# J71t ambient light (CT819) and Smart Cover hall — driver study

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. iPad 5, iPad6,11, S8003 / J71t. **No device access was performed in
this round**; every hardware statement below comes from the saved ADT, the
private IODeviceTree dump, the shared iOS 16.7.10 kernelcache analysis, or the
driver sources already in this repo. No commit was made.

**Implemented since (a9/sensors, 2026-10-23):** the remaining ALS items (§2.7, §4.1) and the polled hall (§4.2 Option A), with the tablet-layer consumers (§4.3). What was decided and what is still a guess is in [`sensors-test-plan.md`](sensors-test-plan.md); the user side is in [`../omarchy/tablet-usage.md`](../omarchy/tablet-usage.md). Differences from the text below: the hall input devices are named `iPad Smart Cover` / `iPad Smart Cover 2`, not `Lid Switch` (logind and Omarchy would suspend on it); line 10 reports `SW_MACHINE_COVER` so it can be told apart from line 0 (`SW_LID`); `gpio-apple-d2365` refuses a line in function mode instead of clearing the bit; the CT819 lux clamps at 5000 and saturates at 1024 counts per integration cycle; `tsl2583.apple_no_pwr_bit` and `tsl2583.apple_int_clear` answer A1 and A2 on a running system; logind is told to ignore a lid switch and `ipad-lid` (not logind) acts on the cover, with suspend opt-in.

Evidence is tagged:

- **[E]** read directly from an artifact (ADT, ioreg dump, kext code, existing
  driver source).
- **[I]** inference that fits the evidence but is not proven.
- **[U]** unresolved; a measurement or a further static check is required.

The two sensors are **independent**: ambient light is an I²C part on `i2c2`
driven by a normal I²C kext, while the cover hall is a **PMIC GPIO event** on
the Dialog D2365 PMU. Neither is an AOP sensor on this board (see
[`aop-sensors.md`](aop-sensors.md), "Ambient light and hall are separate on
this board").

## 0. Result in one table

| | Ambient light | Cover hall |
| --- | --- | --- |
| Part | Apple `CT819` inside `AppleEmbeddedLightSensor.kext` **[E]** | Dialog D2365 PMU GPIO function, lines 0 and 10 **[E]** |
| Bus | I²C2 @ `0x20a112000`, address `0x39` **[E]** | I²C0 @ `0x20a110000`, PMU address `0x3c` **[E]** |
| IRQ | AP pinctrl pin 71, level-low **[E]/[I]** | PMU interrupt line on AP pinctrl pin 76; PMU is itself an interrupt controller with event names **[E]** |
| Register model | TSL2581-compatible: `0x00` control, `0x01` timing, `0x02` interrupt, `0x07` gain, `0x14..0x17` data **[E]** | PMIC GPIO config `0x900 + 2*line`, input banks `0x186..0x188` **[E]** (Linux driver) |
| Calibration | LSCI v2, 8-byte header + `0x40`-byte record, per placement/orientation **[E]** | none; pure level **[I]** |
| Reported to userspace | lux, 0..5000, `in_illuminance` **[E]** | cover open/closed → `SW_LID` (Linux) / `CoverDetached` property (iOS) **[E]/[I]** |
| iOS kext | `com.apple.driver.AppleEmbeddedLightSensor` **[E]** | `com.apple.driver.AppleM68Buttons` (+ `AppleD2365PMU` for the pin) **[E]** |
| Linux status | driver + DT already in `patches/periph`, probed on device **[E]** | not implemented; PMU lines are currently **reserved** **[E]** |

## 1. Artifacts, addresses, reproduction

All kernel addresses are **unslid virtual addresses** in

```text
build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12
SHA256 c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a
Ghidra: kernelcache.release.iPad6,11_12; AARCH64:LE:64:AppleSilicon
```

Kext identities from `build/periph/kexts/prelink.json`:

| Bundle | `_PrelinkExecutableLoadAddr` | Size | Code | Strings |
| --- | --- | --- | --- | --- |
| `com.apple.driver.AppleEmbeddedLightSensor` | `fffffff005320500` | `0x5eb2` | `fffffff005bb6000` + `0x11dd0` | `fffffff005322100` |
| `com.apple.driver.AppleM68Buttons` | `fffffff005928740` | `0x53a6` | `fffffff006c20000` + `0x169a8` | `fffffff005928de8` |
| `com.apple.driver.AppleD2365PMU` | `fffffff005915440` | `0x1e59` | (see `cbat-recon.md`) | — |

Method used this round:

> Private graphics-dependent excerpt omitted from this source snapshot.

Nothing below depends on a device connection.

## 2. Ambient light — Apple CT819 on I²C2

### 2.1 Wiring and identity

Static ADT (`docs/adt/j71t.txt` line 2432 ff.) and the saved private dump
(`build/firmware/private/IODeviceTree.txt` line 1837 ff.) agree:

```text
/arm-io/i2c2                       reg 0xa112000 / 0x1000     -> AP 0x20a112000
/arm-io/i2c2/als1                  reg 0x39                   I²C address 0x39
  compatible = "als,ct819"
  interrupt-parent = <&gpio>       (ADT phandle 30, "gpio,s8000", 208 pins)
  interrupts = <71 1>              AP pinctrl pin 71
  function-int_status = GPIO(0x47, 0x10000)   same pin, flag 0x10000
  alsCalibration = "syscfg/LSCI"   (static dump: unresolved)
  IOHIDPlacementType = 1, IOHIDOrientationType = 1
  probe-for-hardware = 1, noMultiColorSupport = 1, als-colorCfg = 16 zero bytes
  AABNoDimming = 1, useAABPlugin = 1
```

- **[E]** The AP GPIO controller is `/arm-io/gpio` `reg 0xf100000 / 0x100000`
  (AP `0x2100f0000`), `compatible = "gpio,s8000"/"gpio,s5l8960x"`,
  `#gpio-pins = 208`, `interrupt-parent = AIC`.
- **[E]** ioreg shows the node bound as `als1@39 <class AppleARMIICDevice>` and
  the kext personality is `AppleCT819` for `IONameMatch = "als,ct819"`
  (`build/periph/com.apple.driver.AppleEmbeddedLightSensor.json`).
- **[I]** The second `reg` cell (`0x9c4`, the same value on every device of
  `i2c0` and `i2c2`) is a per-bus constant, not a device property. Its units
  are not established; do **not** derive an I²C clock from it.
- **[U]** The `0x10000` flag in `function-int_status` is not decoded. Linux
  uses the `interrupts` specifier and ignores the GPIO function property.

### 2.2 The part is a TSL2581-class ALS

**[E]** `AppleCT819` is one class in `AppleEmbeddedLightSensor.kext`, which
also contains `AppleTSL2561`, `AppleTSL2581`, `AppleISL29023`, `AppleCT821`
and the SPU light-sensor stubs. Its code writes the classic TSL258x register
set and uses the TSL2581 gain encoding, so the Linux `tsl2583.c` register map
applies unchanged.

Direct evidence from the kext:

| Observation | Address |
| --- | --- |
| gain write: `0x87` (i.e. `0x80|0x07`) with an index derived from a multiplier (`<4→0, <12→1, <72→2, else 3`) | `fffffff005bc7164` |
| enable/disable sequence: `0x82` → (`0x81` dummy) → `0x87` → `0x80` → `0x81` → `0x82` (see 2.4) | `fffffff005bc00c4` |
| data read: `0x94`, `0x95`, `0x96`, `0x97` → ch0 = LE16(`0x14/0x15`), ch1 = LE16(`0x16/0x17`) | `fffffff005bc6884` |
| integration-time helper: `cycles*2700 µs`, `TIMING = 256 - cycles` | `fffffff005bc6860`, `fffffff005bc7264` |
| a sibling class clears the ALS interrupt by writing `0x8C` = `0x40` | `fffffff005bc4030` |
| lux computation: leakage/gain normalisation, two-segment `a·ch0 − b·ch1`, fudge/255, clamp 5000.0 (`0x459c4000`) | `fffffff005bc6a60`, `__TEXT.__const` `0x5321c80` |

**[E]** `probe` reads only register `0x80` (control) once and never checks a
part-ID register; matching is purely the DT `compatible`. There is therefore
no ID-based safety net in the iOS driver, and none is needed in Linux either.

### 2.3 `AppleCT819::probe` (`fffffff005bc51b4`)

```text
1. provider = safeMetaCast(parent, AppleARMIICDevice)
2. read 1 byte from register 0x80 (CONTROL)         ; return value not inspected
3. int_status = getProperty("function-int_status")   ; GPIO function, pin 71
   asserted  = int_status->isAsserted()              ; vtable +0x80
   if (error || asserted) -> "ALS_INT is stuck low.. Disabling ALS"
4. placement  = IOHIDPlacementType   (default 1)
   orientation= IOHIDOrientationType (default 1)
5. interrupts_enabled = 1 unless the node has "NoInterrupts"
```

Two consequences for Linux:

- **[E]** iOS expects the interrupt line to be **deasserted (not asserted) at
  probe time**. A line that reads asserted at start is treated as a hardware
  fault and the sensor is disabled.
- **[I]** That is consistent with an **open-drain, active-low** INT output
  (the TSL258x INT pin), which is what the current J71t DT assumes
  (`IRQ_TYPE_LEVEL_LOW`).

### 2.4 Init / enable sequence and registers

The 2581-family enable routine (`fffffff005bc00c4`) programs, in this order
(the CT819 class keeps the same shadow fields at `+0x1d4..+0x1d8` that its own
interval code maintains, but the exact vtable binding of this routine to
`AppleCT819` was not proven — see [U] below):

| # | Register (command byte) | Value | Meaning |
| --- | --- | --- | --- |
| 1 | `0x02` (`0x82`) interrupt | `+0x1d5 & 0xf0` = `0x10` | ALS interrupt **enabled**, persistence 0 |
| 2 | `0x01` (`0x81`) timing | `0xFF` (conditional on `+0x2cf`) | dummy/minimum integration before the first conversion; followed by a helper call (`fffffff005bc8088`) **[U]** exact purpose |
| 3 | `0x07` (`0x87`) gain | `+0x1d7` = gain index 0..3 | 1×, 8×, 16×, 128× |
| 4 | `0x00` (`0x80`) control | `+0x1d6 | 0x02` = `0x02` | ADC enabled (`0x02`) |
| 5 | `0x01` (`0x81`) timing | `+0x1d4` = `256 − cycles` | integration time |
| 6 | `0x02` (`0x82`) interrupt | `+0x1d5` | re-arm |

Disable (same function, `w1 == 0`) clears the interrupt-enable bits
(`+0x1d5 &= ~0x30`), writes `0x82`, and stops the ADC.

**[E]** Control bit 0 (the TSL258x `POWER` bit) is *not* set by this routine:
iOS writes `0x02`, while the Linux driver's measurement state is
`PWR_ON | ADC_ENBL = 0x03`. Either the PMU/rail keeps the part powered, or a
power-state method outside this routine sets bit 0 **[U]**. The Linux driver's
`0x03` is therefore the safer choice and must be validated by a read-back.

Data path (`fffffff005bc6884`):

```text
0x94 -> ch0 low, 0x95 -> ch0 high   (LE16)
0x96 -> ch1 low, 0x97 -> ch1 high   (LE16)
```

Exactly the four bytes the Linux driver reads in
`tsl2583_read_als_int()` (`TSL2583_ALS_CHAN0LO + 0..3`).

### 2.5 Reporting interval and gain (what iOS actually programs)

`AppleCT819::_setALSReportingInterval` (`fffffff005bc7264`) takes the request
in microseconds and computes:

```text
TIMING = 256 - interval_us / 2700          ; 2700 = TSL258x integration cycle
```

with `cycles = interval_us / 2700` clamped to 1..255, and the **gain** chosen
from the calibration record's per-mode gain bytes (1×/8×/16×/128×). It then
writes `"ReportInterval"` back as a property.

**[E]** The integration cycle is **2.7 ms**, not 2.72 ms — `0xa8c` (`2700`) is
the literal used by both the interval and the saturation code. The saturation
threshold used by the lux path is `1024 * cycles - 1` (a 16-bit ADC per
cycle). `_calculateAmbient` (`fffffff005bc6a60`) scales measured counts by
`calibration_cycles / runtime_cycles` and by the gain-factor ratios, subtracts
dark counts and backlight leakage, then applies the two-segment lux curve
`lux = a·ch0 − b·ch1`, multiplies by `fudge / 255.0`
(`__TEXT.__const` `0x5321b80` = `255.0`) and clamps to **5000 lux**
(`0x5321c80` = `5000.0f`).

Defaults visible in `handleStart` (`fffffff005bc5718`):

| Field | Value | Note |
| --- | --- | --- |
| report interval | 50000 µs (20 Hz) | `+0x2d0` |
| integration cycles | 255 (clipped) | overridden by DT `integrationCycles` |
| lux-curve gain | index 1 (8×) | overridden by DT `gainForLuxCurves` |
| published properties | `ALSCh0Gain`, `ALSCh1Gain`, `ALSIntegrationCycles`, `ALSSensorMax = 5000`, `ALSSensorMin = 0`, `ALSSuperFast/Fast/SlowIntegrationTime` | used by the iOS auto-brightness plugin |

### 2.6 Calibration: LSCI v2

The static ADT only has `alsCalibration = "syscfg/LSCI"`. The private dump has
the resolved blob (72 bytes: 8-byte header + one 0x40-byte record — kept in
`build/`, not repeated here by project convention), and the code above pins
the layout:

```text
header (8 bytes)                      record (0x40 bytes, one per
  0  minor    = 0                       placement/orientation)
  1  major    = 2                       0  version = 0
  2  numRecords = 1                     1  placement   (1 on this unit)
  3  recordOffset = 8 (major >= 2)      2  orientation (1 on this unit)
     [major < 2: byte 3 = numLuxCurves, 3  stride/size = 0x40
      byte 6 = record offset]           4  = 1
  4  u16 size = 72 (whole blob)         8  u16 fudge (/255)
  6  (0x3b on this unit) [U]           10  u16 slow integration time (ms)
  7  checksum (unused by CT819) [U]    12  u16 fast integration time (ms)
                                       14  u16 rapid integration time (ms)
                                       16..19 gain multipliers (bytes)
                                       20..63 Q16 coefficients: dark counts,
                                              backlight leakage, per-gain
                                              ratios, two lux-curve segments
                                              and their ratio limit
```

**[E]** `AppleCT819::handleStart` requires at least 8 bytes, reads the record
at `header[3]` (offset 8 on this unit), walks `header[2]` records using the
per-record stride byte, and matches on `(placement, orientation)` taken from
the DT. It then applies the in-record integration times and gains. It does
**not** verify the header checksum (the TSL2581 class does — it owns the
"wrong chksum" string, CT819 does not).

**[E]** The Linux driver adds its own structural checks on the same record:
`record[0] == 0`, `record[3] == 0x40`, `record[4] == 1`
(`patches/periph/0028`). This unit's record satisfies them.

**[E]** This unit's record (all values read by the code above; raw bytes stay
in `build/periph/als-hall/` and in the staged firmware blob):

| Field | Value | Derived |
| --- | --- | --- |
| fudge | 196 | ×196/255 ≈ 0.769 |
| slow integration | 400 ms | 148 cycles, `TIMING = 0x6c` |
| fast integration | 100 ms | 37 cycles, `TIMING = 0xdb` |
| rapid integration | 50 ms | 18 cycles, `TIMING = 0xee` |
| lux-curve gain byte | 8 | gain index 1 → **8×** |
| record header | 1 record, placement 1, orientation 1 | matches the DT |

The `0xdb` initial timing shadow in `handleStart` is exactly the 100 ms fast
mode, which independently confirms the 2.7 ms cycle and the `256 − N`
encoding.

**[E]** The same record also satisfies the Linux driver's own checks and
constants (`ratio offset 0x30`, `ratio scale 256`) in
`patches/periph/0030-iio-light-tsl2583-Add-Apple-CT819-support.patch`: two
independent parsers agree with the blob.

### 2.7 What is proven, inferred, or open

| Claim | Status |
| --- | --- |
| I²C2 `0x20a112000`, address `0x39` | **[E]** ADT + ioreg |
| AP pinctrl pin 71 is the interrupt | **[E]** `interrupts`, `function-int_status` |
| Level-low, open-drain | **[I]** TSL258x INT pin + iOS "stuck low" check; DT already assumes it |
| TSL2581 register map (0x00/0x01/0x02/0x07/0x14-0x17) | **[E]** kext writes/reads |
| LSCI v2 layout and this unit's values | **[E]** code + blob |
| 2.7 ms integration cycle, `TIMING = 256 − cycles` | **[E]** constants |
| lux formula `a·ch0 − b·ch1`, fudge/255, clamp 5000 | **[E]** |
| `CONTROL = 0x02` vs Linux `0x03` (power bit) | **[U]** check read-back on hardware |
| interrupt-clear command for CT819 (`0xE1` special function vs `0x8C`=`0x40`) | **[U]** the CT819 override was not isolated; if the ADC stalls after the first sample, try the sibling's `0x8C/0x40` |
| exact vtable binding of the enable routine to `AppleCT819` | **[U]** same field layout, not proven |

## 3. Smart Cover hall on the D2365 PMU

### 3.1 Wiring

```text
/arm-io/i2c0              reg 0xa110000 / 0x1000     -> AP 0x20a110000
/arm-io/i2c0/pmu          reg 0x3c, compatible = "pmu,d2365"
  device_type = "interrupt-controller", interrupt-controller, #interrupt-cells = 1
  interrupt-parent = <&gpio> (phandle 30), interrupts = <76 1>   ; AP pin 76
  event_name-gpio1  = "halleffect"
  event_name-gpio11 = "halleffect1"
  (also: gpio6 battery, gpio7 bluetooth, gpio8 wlan, gpio10 codec,
         gpio12 dock, gpio13 belfield, gpio16 pcie0)

/buttons                  compatible = "buttons"
  button-names = "hold","menu","voldown","volup","halleffect","halleffect1"
  function-button_halleffect  = 79:GPIO(0x0, 0x0)     ; PMU GPIO line 0
  function-button_halleffect1 = 79:GPIO(0xa, 0x0)     ; PMU GPIO line 10
  button-delay_hold/voldown/volup = 0x124f8           ; none for the halls
  button-priority_voldown = 2
```

- **[E]** phandle 79 is the PMU; the `GPIO(n, flags)` function arguments are
  **0 and 10**.
- **[E]** The `event_name-gpioN` numbering is **1-based** over the same lines:
  `gpio1` ↔ line 0, `gpio11` ↔ line 10 (the buttons node is the direct
  evidence; the aop-sensors report already warned that the two numberings
  differ).
- **[E]** The PMU's own interrupt to the SoC is a single AP pinctrl line
  (pin 76, ADT flags `1`); the PMU then fans out per-line events through its
  own interrupt-controller domain (`IOInterruptController0000004F`).
- **[E]** Live ioreg: `AppleD2365PMU` = 1 instance, `AppleD2365PMUGPIOFunction`
  = 6 instances, `AppleM68Buttons` = 1, `AppleSPUSmartCoverDriver` = 0.

### 3.2 How iOS consumes it: `AppleM68Buttons`, not a cover driver

**[E]** `AppleM68Buttons.kext` (text at `fffffff006c20000`) treats the two
hall lines like any other button:

| Anchor | Address |
| --- | --- |
| class init creates the OSSymbols `hold, menu, volup, voldown, ringerab, **halleffect**, **halleffect1**, hesattach, skey, help, helpdouble` | `fffffff006c2591c` |
| `handleStart` reads `button-names`, then per name `function-button_%s` | `0xfffffff005929042`, `fffffff005929220` (refs `0x6c20af4`, `0x6c21408`) |
| `function-wake_button_%s` (the `wake_button_hold`/`menu` STAT functions) | `fffffff005929272` (ref `0x6c21414`) |
| per-button timing knobs `button-delay_%s`, `button-debounce-on_%s`, `button-debounce-off_%s`, `button-priority_%s` | refs `0x6c20b7c`, `0x6c20b40`, `0x6c20b48`, `0x6c20b50` |
| state publication `ButtonNames`, `ButtonPriorities`, `ButtonStateMask`, `ButtonDownFilteredMask`, `ButtonUnfilteredMask`, `ButtonInterruptTS(ns)`, **`CoverDetached`** | `0x6c21ecc` ff., `0x6c223e4` |
| "No button at interrupt index %d" — an interrupt whose index has no button is an error | `0x6c2407c` |
| dispatch path: builds a 16-byte record (timestamp, index, 0), picks a per-button 32-bit value from two tables (`+0x2a8`/`+0x2d0`, plausibly the filter delay), then queues with a 1000-unit timeout and a comparator callback | `0x6c23f60`–`0x6c24064`, callback `0x6c24088` |

**Interpretation [I]:** iOS binds each hall line as an interrupt-driven
"button" whose value is the pin level, applies the generic button filter
(delay + debounce-on/off + priority), and derives the boolean
`CoverDetached` state that the rest of iOS (display, lock, Smart Cover UI)
consumes. The hall lines are *not* routed through `AppleSPUSmartCoverDriver`
(0 instances) or an AOP Smart Cover usage.

**Caveat [U]:** the exact field behind `CoverDetached` was not tied to a hall
line in this round. The publishing call (`0x6c223e4`) reads a byte field that
is also written by the pin-debug key-chord setup (`debug` property bits
`0x8004`, usage page/usage at `+0x44c`/`+0x44a`), and the two uses may belong
to different objects of the same class. Do not build on "`CoverDetached` ==
line 0" without the iOS `ioreg` observation in 3.3.

**Open [U]:** which HID event/usage the hall finally produces, and whether the
two lines are OR-ed into one cover state or reported separately (a two-magnet
cover can distinguish "closed", "folded behind", "detached"). The public
description of the kext's `EventType`/`UsagePage`/`Usage` dictionaries
suggests a per-button dispatch table, but the table contents were not recovered
in this round.

### 3.3 Polarity and debounce — evidence vs. inference

| Question | Answer | Status |
| --- | --- | --- |
| Which line is the cover? | line 0 = `halleffect` is the primary; line 10 = `halleffect1` is a second hall, most likely the second magnet/edge of the same cover | **[I]** |
| Active low or high? | Active **low** when the magnet is present (cover closed); line idles high with the cover open | **[I]** — the probe-time "asserted is a fault" pattern and typical open-drain hall outputs; **not measured** |
| Debounce | iOS uses the per-button `button-delay_*`/`button-debounce-on_*`/`button-debounce-off_*` mechanism; the ADT sets **no** hall-specific delay, so the kext defaults apply | **[E]** mechanism, **[U]** values |
| Does the level latch or pulse? | Unknown; the PMU event logic may report edges only | **[U]** |

**Measurement (must run on hardware before writing the driver):**

1. On iOS, with the cover open and closed, capture
   `ioreg -p IOService -l -w 0` twice and diff the `AppleM68Buttons` /
   `AppleD2365PMU` nodes (`ButtonStateMask`, `ButtonUnfilteredMask`,
   `CoverDetached`, `ButtonInterruptTS(ns)`). This gives the state *and* the
   timing of the filter without guessing.
2. On Linux (after un-reserving the lines, see 4.2), read the PMU GPIO config
   and input registers with the cover open/closed and with the magnet moved
   slowly across the edge. `0x900 + 2*0` (line 0) and `0x914` (line 10) give
   the mode bits; `0x186`/`0x187` give the input levels.
3. Time 20 open/close cycles to size the debounce (start at 50 ms; iOS's other
   buttons use 75 ms delays).

## 4. Linux plan

### 4.1 Ambient light — mostly done, needs validation

Already in the tree (do not redo): bindings `0029`, driver `0030`, KUnit
calibration tests `0031`, J71t wiring `0035`. Per `round3.md` the integration
kernel with `79aef2ef96` loaded this unit's LSCI v2 and found the sensor at
0.75 s.

Remaining work, in order:

1. **Verify the power bit.** Read back `0x00` after the driver's
   `PWR_ON|ADC_ENBL` write. If bit 0 does not stick, the part is rail-powered
   and the driver must not treat the write as an error.
2. **Verify the interrupt-clear path.** The driver currently clears with the
   TSL258x special function (`0xE1`). If the second sample never arrives, try
   the sibling-class clear (`write 0x8C = 0x40`).
3. **Lux accuracy.** Compare `in_illuminance_input` against a phone light
   meter at 10/100/1000 lux, cover the sensor, and check the 5000 lux clamp.
   The calibration is 8× gain / 400 ms reference, fudge 196/255.
4. **Interval.** Keep the default 20 Hz (`report_interval` 50 ms →
   `TIMING = 0xee`) but do not advertise untested rates; iOS itself runs
   20 Hz by default.
5. **DT** (already present, no change needed):

```dts
&i2c2 {
	status = "okay";
	light-sensor@39 {
		compatible = "apple,ct819", "amstaos,tsl2581";
		reg = <0x39>;
		interrupts-extended = <&pinctrl_ap 71 IRQ_TYPE_LEVEL_LOW>;
		firmware-name = "apple/j71t-LSCI.bin";
	};
};
```

The calibration blob must stay a per-device firmware file
(`request_firmware_direct()`), never a public DTS property and never a write
back to syscfg.

### 4.2 Cover hall — two options, one decision gate

**Option A (preferred if the pin is readable): PMU GPIO + `gpio-keys`.**

The Linux `gpio-apple-d2365` driver already exposes 24 lines through the PMI
regmap (`patches/periph/0011`), reading inputs from `0x186 + line/8` and
config from `0x900 + 2*line`. Today the J71t DT **reserves lines 0..12 and
14..23**, which is exactly why the halls are unavailable:

```dts
periph_pmu_gpio: gpio {
	compatible = "apple,d2365-pmic-gpio";
	gpio-controller;
	#gpio-cells = <2>;
	gpio-reserved-ranges = <0 13>, <14 10>;   /* only line 13 usable */
};
```

Change to leave lines 0 and 10 out of the reserved ranges, then:

```dts
/ {
	lid-switch {
		compatible = "gpio-keys-polled";   /* see note below */
		poll-interval = <50>;

		lid {
			gpios = <&periph_pmu_gpio 0 GPIO_ACTIVE_LOW>;
			linux,input-type = <EV_SW>;
			linux,code = <SW_LID>;
			debounce-interval = <50>;
		};
	};
};
```

- **Polled, not interrupt-driven, until an irqchip exists.** `gpio-apple-d2365`
  implements a plain `gpio_chip` with no `irq_chip`, so `gpio-keys` (which
  needs an interrupt) cannot bind. `gpio-keys-polled` works today at the cost
  of a 20–50 ms poll and **no wake from suspend**.
- Gate: the pin must not be in "function" mode. `d2365_gpio_get_direction()`
  returns `-EINVAL` when config bit 4 (`FUNCTION`) is set, and the reserved
  ranges suggest the firmware left these pins owned by the PMU event logic.
  Measure first (3.3 step 2). If line 0 reads as function-mode, use Option B.
- `EV_SW`/`SW_LID` is the correct abstraction: it is a state, not a key, and
  `systemd-logind`, Hyprland and `iio-sensor-proxy` all understand it.
- 50 ms poll/debounce is a starting point, to be replaced by the measured
  value. Polling also means the lid state is available immediately at boot,
  which an edge-only IRQ path does not give you.

**Option B (if the pin is function-mode, or when wake is required): model the
PMU event domain.**

Extend `gpio-apple-d2365` (or a small MFD child) with an irqchip that matches
the ADT's `interrupt-controller`/`#interrupt-cells = 1` and the
`event_name-gpioN` map, request the parent IRQ (AP pin 76), and decode the
PMU's per-line event/status registers. A child input device then reports
`SW_LID` from the level, and the same irqchip lets a plain `gpio-keys` node
work and makes the lid a wake source. This mirrors iOS exactly (event index →
line → state) but needs the PMU event registers, which are **not** in this
round's evidence; they would come from `AppleD2365PMU.kext` (the same analysis
that produced `cbat-recon.md`).

Decision gate: after the 3.3 measurement, Option A if `config & 0x18 == 0x08`
(input mode) and the input register tracks the magnet; Option B otherwise.
Do **not** repurpose line 0 as an output or clear its function bits blindly —
those bits also feed the PMU's wake logic.

**Wake behaviour.** Both options need `wakeup-source`, and the parent chain
(PMU IRQ → AP pinctrl pin 76 → AIC) must be a valid wake path. That is a
separate suspend/resume task; do not enable wake before the switch itself
reports correctly.

### 4.3 Userspace hooks (Omarchy)

**Brightness from the ALS.** The kernel gives
`/sys/bus/iio/devices/iio:deviceN/in_illuminance_input` (lux) plus the
calibration attributes `in_illuminance_calibscale`,
`in_illuminance_calibscale_available` and `in_illuminance_integration_time`.
Two viable consumers:

- `iio-sensor-proxy` already ships in the Omarchy image (the autorotate work
  uses it) and exposes `net.hadess.SensorProxy.AmbientLightLevel`; a small
  user unit can watch that property and call `brightnessctl`.
- or read the sysfs file directly at 1 Hz, like `ipad-autorotate` does for the
  accelerometer (`docs/omarchy/tablet-usage.md`).

Policy that will not fight the user: only adjust when the ambient level
changes by more than ~20 % and more than ~2 s apart, clamp to 5..100 % of the
panel range, keep a manual-override timestamp (any brightness key press
suspends auto-adjust for ~30 s), and never go below the panel's minimum.
Suggested shape for a future `ipad-als-brightness` unit, mirroring
`ipad-autorotate`:

```text
/etc/systemd/user/ipad-als-brightness.service   (ConditionPathExists on the IIO lux file)
~/.local/bin/ipad-als-brightness                (reads lux, hysteresis, calls brightnessctl)
```

**Lid.** With `SW_LID` working, the whole stack is standard:

```ini
# /etc/systemd/logind.conf.d/10-ipad-lid.conf
[Login]
HandleLidSwitch=suspend
HandleLidSwitchExternalPower=suspend
LidSwitchIgnoreInhibited=no
```

- Lock before sleep with a `systemd-sleep` hook
  (`/usr/lib/systemd/system-sleep/10-hyprlock.sh`) or `hypridle`'s
  `before_sleep_cmd = hyprlock`, so the session is locked when the cover opens.
- `hypridle` can also listen for `lidswitch` via logind's `PrepareForSleep`
  signal if suspend is not wanted while docked.
- The cover-open event must be able to wake the PMU path; until that is
  validated, expect wake-from-lid to need a key press.

### 4.4 Test plan (commands only — none were run in this round)

```sh
# 1. ALS identity and first sample (Linux, after boot)
sudo cat /sys/bus/iio/devices/iio:device*/name
cat /sys/bus/iio/devices/iio:device*/in_illuminance_input
cat /sys/bus/iio/devices/iio:device*/in_illuminance_calibscale

# 2. ALS interrupt behaviour: count IRQs while covering/uncovering
grep -i -E 'ct819|tsl2583|71' /proc/interrupts

# 3. Hall measurement (Option A gate): PMU config + input registers.
#    Register addresses are 16-bit; the bus number is whatever i2c0
#    enumerated as (check /sys/bus/i2c/devices/i2c-*/name).
sudo i2ctransfer -y 0 w2@0x3c 0x09 0x00 r2   # line 0 config  (0x900)
sudo i2ctransfer -y 0 w2@0x3c 0x09 0x14 r2   # line 10 config (0x914)
sudo i2ctransfer -y 0 w2@0x3c 0x01 0x86 r2   # input bank 0 (lines 0..7)
sudo i2ctransfer -y 0 w2@0x3c 0x01 0x87 r2   # input bank 1 (lines 8..15)

# 4. Hall event once wired
sudo evtest /dev/input/eventX                   # expect EV_SW / SW_LID 0/1
sudo libinput debug-events                      # lid switch events

# 5. Suspend path (only after 4 is stable)
systemctl suspend
journalctl -b -u systemd-logind -n 50
```

Scripted, repeatable steps belong in `scripts/periph/`, not in this document.

### 4.5 Gates and risks

| Gate | Why it can block |
| --- | --- |
| ALS power bit / interrupt clear | A wrong `CONTROL` or clear command gives one sample and then silence |
| PMU line 0 in function mode | Option A impossible without touching PMU function bits |
| No `irq_chip` in `gpio-apple-d2365` | Option A is polled-only: no wake from suspend until an irqchip exists |
| PMU event register map | Option B needs new static evidence from `AppleD2365PMU.kext` |
| Wake path validation | Lid-open wake can hang the resume path if the PMU IRQ is not a wake source |
| Calibration storage | LSCI must stay a per-device firmware file; never write syscfg |
| Reserved GPIO ranges | Un-reserving lines 0/10 changes a shared DT property used by the charger/backlight work — retest those |

## 5. Open questions

| ID | Question | Cheapest way to close it |
| --- | --- | --- |
| A1 | Does CT819 need `CONTROL` bit 0 (power) set? | read-back on Linux, one boot |
| A2 | Which interrupt-clear does CT819 use (`0xE1` vs `0x8C`=`0x40`)? | two-sample test, then swap |
| A3 | Exact lux error vs. reference | phone light meter, 3 points |
| A4 | Is the ALS INT really level-low? | scope or `/proc/interrupts` count with the line forced |
| H1 | Hall polarity | measure with cover open/closed (3.3) |
| H2 | Are lines 0/10 input-readable or function-mode? | PMU register read (3.3 step 2) |
| H3 | Debounce/filter values iOS uses | iOS `ioreg` diff during open/close |
| H4 | One cover state or two (0 and 10) | iOS event observation; then decide OR vs two inputs |
| H5 | Does the PMU expose per-line event registers we can use? | `AppleD2365PMU.kext` analysis (same method as `cbat-recon.md`) |
| H6 | Is lid-open a wake source through AIC? | suspend test after H1–H4 |

## 6. Relation to the existing documents

- [`aop-sensors.md`](aop-sensors.md) §"Ambient light and hall are separate on
  this board" is confirmed and extended here: the ALS is I²C/CT819, the hall is
  PMU GPIO, and neither is an AOP endpoint. That document's warning about
  `AppleSPUSmartCoverDriver` is confirmed by its zero instance count.
- [`round3.md`](round3.md) records the on-device ALS probe; this document
  supplies the register/calibration detail behind it and the remaining
  validation steps.
- [`cbat-recon.md`](cbat-recon.md) documents the D2365 analysis method that
  Option B would reuse.
- The hall has no earlier report; `PLAN.md` should gain a line for
  "cover hall → SW_LID" once H1/H2 are answered.
