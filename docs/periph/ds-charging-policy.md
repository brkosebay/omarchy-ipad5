# J71t USB charging policy: what iOS does, what our driver does, what it should do

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Design study for the `apple-j71t-charger` / `apple-tristar-power`
pair while the iPad is permanently plugged into the owner's Mac.

**Scope.** No device access, no register writes, no commits, no syscfg/NVRAM
writes. Sources: the shared Ghidra analysis of
`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12` (image base
`fffffff005298000`, 90,131 functions), the live
`build/firmware/private/IOService.txt` dump, this unit's ADT
(`docs/adt/j71t.txt`), tonight's `build/omarchy/logs/netconsole-boot8-nvme.log`
(kernel `round8e-handoff-bfe22c60a0`), and the driver sources in the
`linux-a9-src` volume (`/src/wt/{periph,boot,integ}/drivers/power/supply/`:
the two files are byte-identical on `a9/periph`, `a9/boot`, `a9/integ`,
`a9/tri33` and the detached `bootr8e-check`, and identical to
`scripts/periph/kernel/*.c`, 666 + 559 lines).

Marks: **[V]** verified in code, ADT, live state or log; **[I]** inferred from
consistent evidence, not proven. Every [V] carries the address, file:line or
log:line it comes from.

---

## Bottom line

1. **The 2.4 A is a platform constant, not a negotiation.** **[V]**
   `AppleARMPMUCharger::start` (`fffffff005f15504`) reads the DT property
   `usb-input-limit-max` into `+0x1f0` (hard default 1000 mA) and this unit's
   ADT says `usb-input-limit-max: 0x960` = 2400 (`docs/adt/j71t.txt:3273`).
   Nothing about the Mac, the cable or bMaxPower raises it; iOS simply uses the
   board maximum for a "usb host" source.
2. **The PMIC only gets three registers, in a fixed order.** **[V]** input
   limit `0xa03`, battery charge limit `0xa08`, and `0xa00` bit 1 (input
   disable) which iOS only asserts for targets below 75 mA. Order:
   `0xa03` read → code from the CBAT record → `0xa00[1]` → `0xa03` write
   (`fffffff006bbfed0`), and `0xa08` written separately
   (`fffffff006bbfd64`). `0xa00[1]` is not used as a transaction lock.
3. **D+/D− and VBUS.** **[V]** The charger path touches no USB PHY, DWC2 or
   VBUS-source register — only the I²C PMIC at `0x20a110000/0x3c`. The only
   D+/D− manipulation in the whole charging path is Tristar: register `0x01`
   (route a pin to the PMU ADC) and register `0x03` (a 40 ms D+/D− pulse train;
   **[I]** BC1.2 DCP/divider signalling), both in the attach-time detector
   (`fffffff005ff5a08`). Our driver does `0x01` only, and only while the gadget
   is unconfigured (`apple_tristar_power.c:190`).
4. **Our `-34` is not a fault, it is our policy following the USB link state.**
   **[V]** `j71t_policy_limit()` (`apple_j71t_charger.c:170`) returns the
   supplier's instantaneous `CURRENT_MAX`; the gadget reports 0 mA while
   suspended and 100 mA before a configuration is selected
   (`drivers/usb/gadget/udc/core.c:96`). A single bus suspend therefore
   revokes the 2.4 A override, restores the handoff snapshot — which asserts
   input suspend — and later re-enables 500 mA. That is a 2.4 A → 0 → 0.5 A
   step, the only charger action that can couple into the USB link.
5. **Our `-19` is a real bug.** **[V]** `bq27xxx` reports `PRESENT=0` whenever
   the cached flags sample failed (`bq27xxx_battery.c:2413`, patch 0053), and
   `j71t_battery_safe()` maps that to `-ENODEV` (`apple_j71t_charger.c:203`),
   which latches `faulted` forever. Two occurrences in tonight's log, both
   immediately after `battery cache refresh failed: -5`
   (log:69551→69555, log:124936→124957). After 01:30:39 the charger never
   re-armed again — 32 minutes of log with nothing but the 30 s latch message.
6. **The `-11` in that window is not the charger.** **[V]** The only `-11` is
   `configfs-gadget.g1 gadget.0: acm ttyGS0 can't notify serial state, -11`
   (log:107964) — the DWC2 non-L0 `usb_ep_queue()` rejection already diagnosed
   in [usb-resume.md](usb-resume.md). No `charging request revoked: -11` or
   `charging policy deferred: -11` exists in the entire log.
7. **Safe policy:** hold 2400 mA input / 2500 mA battery as iOS does on this
   exact port, make the budget immune to USB suspend/reset, never suspend the
   input to change a limit, treat gauge and I²C errors as transient, latch only
   on register-ownership failure and auto-clear that latch. Section 3.
8. **Concrete changes:** six in `apple_j71t_charger.c`, one required (plus one
   optional) in `apple_tristar_power.c`, one DT property pair, one systemd
   simplification. Section 4.

---

## 1. How iOS decides the USB input current

### 1.1 The ceiling comes from the device tree

**[V]** `AppleARMPMUCharger::start` (`fffffff005f15504`) initialises
`*(u32 *)(self + 0x1f0) = 1000`, then overwrites it from the `usb-input-limit-max`
property of its own node. The J71t ADT has `usb-input-limit-max: 0x960`
(`docs/adt/j71t.txt:3273`), i.e. 2400 mA, next to
`usb-input-limit-calibration: "syscfg/CBAT/0x18"` (`:3278`) and
`min-charge-capable-current: 0x3e8` (`:3285`, 1000 mA).

**[V]** The same node's `usb-input-limit-calibration` is read by the D2365
attach path `fffffff006bbf868`, which requests exactly 0x18 (24) bytes and
builds the calibration object the input-code getter later consults; a bad
record logs `AppleD2365PMUPowerSource::attachCharger Warning: bad
usb-input-limit-calibration data/format` (`fffffff005916c76`). This is the same
24-byte record we stage as `apple/j71t-CBAT.bin`.

So the board says "this PMIC may take up to 2.4 A from USB". The remaining
question is only *when* iOS chooses that value instead of 500 mA.

### 1.2 Source classification: Tristar + IOAccessoryManager

**[V]** Attach detection runs in `accessory_determine_usb_charger`
(`fffffff005de8328`): it reads PMU ADC channels 1–5 through the pin-monitor
object, and for the D+/D− pair calls `accessory_brick_id_from_dp_dm_mv`
(`fffffff005de44c4`) with thresholds 1500 / 2200 / 2880 mV — a 3×3 grid
yielding brick IDs 0–9. Our Linux table
(`apple_tristar_brick_current()`, `apple_tristar_power.c:166`) uses the same
thresholds (1500/2200/2880) and the same 3×3 ordering, so the two agree.

**[V]** The pin measurement itself is `tristar_measure_usb_pin_voltage`
(`fffffff005ff5a08`): it reads DX_CONTROL (register `0x01`), substitutes the
monitor nibble (`(saved & 0xf0) | 0x0c/0x0d`, or `(saved & 0x0f) | 0xc0/0xd0`
for the other orientation — bit-for-bit what `apple_tristar_pin_mv()` writes,
`apple_tristar_power.c:151-153`), writes `0x01`, reads it back, starts the PMU
ADC, and for DCP/divider monitor types 4/5 also writes register `0x03`
(`dcp_ctrl`) with `0x09/0x0a/0x0b/0x0c` for 40 ms before clearing it. It
restores `0x01` from the saved value afterwards.

**[I]** Register `0x03` is BC1.2 DCP signalling: it drives the D+/D− divider or
short that a dedicated charger is detected by. Our driver never writes `0x03`,
and must not while a configuration is selected.

**[V]** Tonight's iPad-on-Mac state, from the live dump, is an ordinary SDP:
`IOAccessoryUSBPowerSourceDetect` published
`{"class"="IOAccessoryPowerSourceItemBrickID", "brick id"=0, "d+ voltage"=39,
"d- voltage"=39, "adapter description"="usb brick", "current limit"=0}` and
`{"class"="IOAccessoryPowerSourceItemUSB_ChargingPort", "charging port
type"="sdp", "current limit"=0}` (`IOService.txt:13560`). 39 mV on both lines
is the Mac's 15 kΩ pull-downs: no brick, no DCP, plain data port.

### 1.3 A USB host gets the platform maximum

**[V]** Next to those zero-limit items, `IOAccessoryUSBConnectShim` publishes
`{"class"="IOAccessoryPowerSourceItemUSBHost", "adapter description"="usb
host", "current limit"=2400, "adapter family"=18446744073172697088}`
(`IOService.txt:13560`). That family code is exactly `0xffffffffe0004000`, the
constant registered for `IOAccessoryPowerSourceItemUSBHost`
(`fffffff005de6e38`), and an `IOAccessoryPowerSourceItemRestrictions` item with
`"max current"=2400` from `IOAccessoryDock9Pin::handleAccessoryDetect` sits in
the same list. `IOService.txt:13498` and `:13534` then show what the PMU saw:
`AppleRawAdapterDetails` / `AdapterDetails` =
`{"Description"="usb host", "AdapterVoltage"=5000, "Current"=2400,
"Watts"=12, "PMUConfiguration"=2400}`, with `ChargerConfiguration` 2500,
`IsCharging` Yes, `Amperage` 1704, `Voltage` 3918.

**[I]** The item carries 2400 because that is the platform ceiling; the
restriction can only lower an item's current
(`accessory_apply_source_restrictions`, `fffffff005de527c`, only assigns when
the restriction is smaller), so the 2400 is either the item's own base or a
class default. Either way the outcome is fixed: **a USB host that is not a
BC1.2 charger is charged at `usb-input-limit-max`**. The `iOS` value is not
derived from bMaxPower, from an Apple brick ID, or from a vendor request.

### 1.4 The USB stack's own budget is a separate accounting domain

**[V]** IOUSBDeviceFamily does compute a descriptor-based budget, and it is
*not* 2400: `usb_device_set_configuration_budget` (`fffffff0060e4528`) takes
`bMaxPower` (`usb_device_configuration_maxpower_units`, `fffffff0060e379c` —
default 250 units for USB 2.0 speeds, 112 units for SuperSpeed when the
descriptor has none), multiplies by 2 (USB 2.0) or 8 (SuperSpeed) and stores
it at `+0x104`. `usb_device_resolve_current_limit` (`fffffff0060e4074`) then
reports, per charger type: type 0/5 = the stored base plus the descriptor
budget when either is zero, otherwise base + 500; type 1 = a single stored
value at `+0xfc`; types 2/3/4 = 500 mA; unknown = 0 — all clamped by a cap at
`+0x108`. `usb_device_handle_suspend` (`fffffff0060e54d8`) zeroes the USB side
while the bus is suspended.

**[V]** The Apple "extra power" request is `FUN_fffffff005f4c8a4` (the
`RequestExtraPower` method, string at `fffffff0054ac015`): it asks the provider
for `requested + 500 mA` (types 1/6) or `requested + 900 mA` (type 7) and
returns the grant above that baseline, refusing if the allocation did not
reach 501 / 901 mA. This is the 500 mA → 1.0/1.4 A class negotiation, not the
path to 2.4 A.

**[V]** Our Linux gadget advertises 500 mA: the log line
`owner authorizes 2400 mA input; detected allowance 500 mA` can only be
produced when `budget.current_ma >= 500` (`apple_tristar_power.c:232-233`
caps to 500, and `core.c:96` derives it from `usb_gadget_vbus_draw()`).

**[I]** Conclusion: two independent numbers exist on iOS. The USB stack
accounts 500 mA for the link, and the accessory/PMU domain grants the platform
maximum for a "usb host". Linux currently has only the first one, which is why
this project needed the owner sysfs assertion at all.

### 1.5 What reaches the PMIC, and in what order

**[V]** Input current, `d2365_set_calibrated_input_current_ma`
(`fffffff006bbfed0`), called through the power-source vtable `+0xd8`
(`fffffff006fd48d0`):

1. read `0xa03` (one byte, discarded);
2. `AppleD2365PMUPowerSource_getInputCurrentSetting` (`fffffff006bbff6c`)
   with calibration enabled: the CBAT object at `+0x58` maps target mA, then
   `code = ((mA * 8 - 600) & 0xfff8) / 100`, 0 below 75 mA, 0xff at ≥ 3262 mA;
3. `d2365_update_a00_bit1` (`fffffff006bc0038`): read-modify-write `0xa00`,
   **only** bit 1 = (target < 75 mA), all other bits preserved;
4. write `0xa03` = code.

**[V]** Battery charge current, `AppleD2365PMUPowerSource_setChargeCurrentLimit`
(`fffffff006bbfd64`): clamp to 3150 mA, `code = ((mA >> 1) * 0x147b) >> 17`
(≈ 50 mA steps), cap at `0xa0c[5:0]` latched at attach
(`fffffff006bbf868`), write `0xa08`.

**[V]** For this unit 2400 mA → `0xba` (75 + 186 × 12.5 = 2400) and
2500 mA → `0x32`, which is exactly what our driver logs
(`owner override budget 2400 mA: input 0xba, battery limit 2500 mA`), and
500 mA → `0x20` (475 mA), which our driver also logs
(`source budget 500 mA: input 0x20, battery limit 400 mA`).

**[V]** Nothing else is written on the charging path: no `0xa00` full-byte
write, no charge voltage, no watchdog, no interrupt mask, no cap register, no
factory data. `0x18a` is read for status (bits 1:0 charging, bit 2 termination,
bit 4 timeout).

### 1.6 Does any of it touch D+/D− or VBUS?

| Path | Register | Effect on the link | Evidence |
|---|---|---|---|
| PMU input limit | D2365 `0xa03` | none (input current loop) | **[V]** `fffffff006bbfed0` |
| PMU battery limit | D2365 `0xa08` | none | **[V]** `fffffff006bbfd64` |
| PMU input disable | D2365 `0xa00` bit 1 | gates the charger input sink; **[I]** does not gate port VBUS | **[V]** RMW only bit 1 `fffffff006bc0038`; **[I]** electrical role |
| Pin mux | Tristar `0x01` | **replaces D+/D− with the ADC** | **[V]** `fffffff005ff5a08` |
| BC1.2 DCP | Tristar `0x03` | **drives D+/D− divider/short** | **[V]** `fffffff005ff5a08` (types 4/5 only); **[I]** BC1.2 meaning |
| VBUS source | — | Mac supplies it; nothing on our side switches it | **[V]** `docs/periph/ds-usb-host.md` host/device switch is a separate, unused path |

**[V]** Therefore a charger current-limit change cannot break the link
electrically by itself. The mechanisms that can are (a) the Tristar mux/BC1.2
writes, and (b) the host port's own over-current behaviour reacting to what we
draw. **(b)** is exactly what repeated 2.4 A → 0 → 0.5 A steps provoke, and it
is the one we currently do on every USB budget flap.

---

## 2. What our driver actually does

### 2.1 Error map

| Message | Emitted at | errno | Real trigger |
|---|---|---|---|
| `charging request revoked: -34` | `apple_j71t_charger.c:374` | `-ERANGE` | `j71t_guard()` fails: source allowance < active budget (`:254-256`), battery temp outside 10–40 °C or voltage outside 3.0–4.2 V (`:206-213`), or `0xa0c[5:0] < 50` (`:267-270`) |
| `charging policy deferred: -34` | `:341` | `-ERANGE` | the same guard triggers while no budget is active, or `j71t_choose_budget()` with an allowance < 500 mA (`:98-106`, surfaced at `:403-404`) — logged once per 30 s (`:331`) |
| `charging request revoked: -19` | `:374` | `-ENODEV` | `battery PRESENT != 1` (`:201-205`), which `bq27xxx` returns **for any failed flags sample** (`bq27xxx_battery.c:2413`) |
| `charging request revoked: -11` | `:374` | `-EAGAIN` | battery provider missing, gauge cache not ready (`-ENODATA` remapped at `:217`), or supplier `psy` absent (`:150`, `:162`). **Did not occur in tonight's log.** |
| `charging policy fault latched; guarded owner retry required` | `:332-333` | — | any of the above whose errno is outside `{-ENOLINK, -ERANGE, -EBUSY, -EAGAIN}` (`:281`, `:372`): `-ENODEV` from the battery presence mapping, `-EIO`/`-ETIMEDOUT` from I²C or HDQ, `-ENODEV` from a Tristar revision mismatch |
| `owner input override revoked` | `:188` | — | every path that drops `user_limit_ua`, including a transient 0 mA allowance (`:353-354`) |

### 2.2 The `-34` sequence is our own churn

**[V]** `build/omarchy/logs/netconsole-boot8-nvme.log`:

```
107943  43.682  owner authorizes 2400 mA input; detected allowance 500 mA
107944  43.698  owner override budget 2400 mA: input 0xba, battery limit 2500 mA
107961  48.414  owner input override revoked            <- allowance fell below 500 mA
107962  48.421  charging request revoked: -34
107963  48.427  charging policy deferred: -34; retry scheduled
107964  49.133  acm ttyGS0 can't notify serial state, -11   <- unrelated
107965  49.158  source budget 500 mA: input 0x20, battery limit 400 mA
```

**[V]** `j71t_policy_limit()` runs *before* the guard (`:352-354`), so the
revocation is decided by the supplier's `CURRENT_MAX` alone. That value is
produced by `apple_tristar_budget()` (`apple_tristar_power.c:220-242`), which
returns **0 when `budget.suspended`**, **`min(current_ma, 100)` before a
configuration is selected**, and `min(current_ma, 500)` when configured. All
three come from `gadget_current_snapshot()` (`drivers/usb/gadget/udc/core.c:96`);
`usb_gadget_set_state()` clears `current_configured` and sets
`current_bus_connected = (state == USB_STATE_DEFAULT)`, `current_base_ma = 100`
for every state ≤ DEFAULT (`core.c:1330-1336`), and
`usb_gadget_current_budget_suspend()`/`USB_STATE_SUSPENDED` zero the budget
(`core.c:198-212`, `:1337-1340`).

> Private graphics-dependent excerpt omitted from this source snapshot.

**[V]** The consequence is not cosmetic: `j71t_restore()` (`:221-245`) asserts
`0xa00` bit 1 *before* writing the snapshot and clears it afterwards, so the
PMIC input goes to zero and back. Then `j71t_enable()` (`:300-310`) repeats the
same suspend/write/resume dance to install 500 mA. One USB suspend therefore
produces two full input-current interruptions.

**[V]** After the 500 mA state is installed the owner request is gone for good:
nothing re-arms it, and `linux-a9-charger-limit.service` is `Type=oneshot`
(`scripts/omarchy/rootfs/files/etc/systemd/system/linux-a9-charger-limit.service`).

### 2.3 The `-19` latch is a gauge error misread as a removed pack

**[V]** Two occurrences, identical ordering:

```
69551  23:56:33  w1_slave_driver 01-000000000000: battery cache refresh failed: -5
69552  23:56:33  charging request revoked: -19
69555  23:56:33  charging policy fault latched; guarded owner retry required
...
124936 01:30:38  w1_slave_driver 01-000000000000: battery cache refresh failed: -5
124938 01:30:38  charging request revoked: -19
124957 01:30:39  charging policy fault latched; guarded owner retry required
```

**[V]** The chain is exact:
`bq27xxx_battery_refresh_cache()` stores the failed safety sample in
`cache.flags = -EIO`; `bq27xxx_battery_get_cached_property()` returns
`PRESENT = (cache.flags < 0) ? 0 : 1` (`bq27xxx_battery.c:2413`, introduced by
patch `0053-power-supply-bq27xxx-distinguish-cache-warmup-from-r.patch`);
`j71t_battery_safe()` turns that 0 into `-ENODEV` (`:203`); the guard returns
it (`:257-258`); the policy work classifies it as non-transient (`:372-373`)
and sets `faulted`; `j71t_policy_log()` then prints the latch line every 30 s
(`:326-333`) and `j71t_enable()` is skipped forever (`:385`).

**[V]** The latch is only cleared by a *positive* `input_current_limit` write
(`:491`). From 01:30:39 to the end of the log at 02:02:54 the charger produced
96 more latch lines and no state change (only one `[omarchy-rd]` re-print of
the earlier `-19`): 32 minutes with no charging policy on a tablet whose root
filesystem is served over the same USB cable.

**[V]** The latch rule is broader than the design intent. `j71t_enable()` and
`j71t_policy_work()` fault on *any* errno outside a four-value allow-list, so a
transient Tristar I²C error also latches: `apple_tristar_get_property()`
returns the `apple_tristar_update()` error together with the cached value
(`apple_tristar_power.c:316`, `:335`), and `j71t_online()` passes it through
(`:154`). One `-EIO` or `-ETIMEDOUT` on I²C0 stops charging until reboot.

### 2.4 The `-11` you saw is the DWC2/ACM bug

**[V]** `grep -c 'apple-j71t-charger.*-11'` over the whole 127 k-line log is 0.
The `-11` at 49.133 is `configfs-gadget.g1 gadget.0: acm ttyGS0 can't notify
serial state, -11`, the `dwc2_hsotg_ep_queue()` non-L0 rejection fixed by
`078d2de49b` and `46864bf126` (see [usb-resume.md](usb-resume.md)). It is a
*consequence* of the link state, not a charger error, and it does not latch.

---

## 3. Policy for a tablet that lives on a Mac

### 3.1 Target

**[V]** iOS is configured for, and reports, charging at that level on this
port and cable: `AdapterDetails` 2400 mA / 12 W, `IsCharging` Yes, 1704 mA into
the pack, 3918 mV, and no charger-side fault in the dump. There is therefore no
evidence that 2.4 A steady state is unsafe here. The hazard is the
*transitions*, not the level.

**Recommended steady state:** input 2400 mA, battery 2500 mA, held steady.
**Recommended floor:** 500 mA (475 mA programmed) only while VBUS is present
but no host has enumerated us, or after a deliberate step-down.
**[V]** Note `min-charge-capable-current: 0x3e8` (1000 mA) in the ADT: 475 mA
is below the platform's own "charge capable" threshold, which matches the
observed net drain at that setting.

### 3.2 Rules

1. **The enumerated budget, not the instantaneous link state, bounds the
   input.** Hold the last budget through bus suspend and through a reset that
   transiently reports 100 mA. Fall back to 500 mA only after the low
   allowance has persisted for a grace window (5 s), and to 0 only when VBUS
   is gone.
2. **Never drop VBUS current to change a limit.** Match iOS: write `0xa08`
   then `0xa03` directly. Use `0xa00` bit 1 only for a true disable
   (target 0 / below 75 mA). This removes two 2.4 A → 0 steps per revocation.
3. **Never revoke the owner ceiling on a transient.** `user_limit_ua` is
   cleared by an explicit `0` write or by VBUS loss. A dip to 0 mA, a gauge
   error or a Tristar timeout must not clear it.
4. **Never latched-until-reboot.** `faulted` becomes a bounded backoff
   (60 s) with automatic retry, and only register-ownership failures
   (readback mismatch of a register we wrote) may set it.
5. **Never touch D+/D− while configured.** Keep `!connected && !host_grant`
   (`apple_tristar_power.c:190`); never write Tristar `0x03`; do not add a
   "re-detect on resume" path.
6. **Never step down while the link is up** unless the pack is at a guard
   limit (temperature, termination, timeout). The root filesystem is on NBD
   over this cable: a link drop is not a degraded mode, it is a dead system.
7. **Thermal guard with hysteresis.** The current 10–40 °C window
   (`:206`) is a test-policy choice, not iOS fidelity; this pack's own lifetime
   maximum is 46.4 °C (`IOService.txt:13521`, `MaximumTemperature` 464). Use
   trip 45 °C / clear 42 °C, and treat a trip as a *step-down*, not a revoke.

### 3.3 What not to do

- Do not lower the target to "be safe". 500 mA is proven to drain this
  tablet, and the iOS steady state at 2.4 A is proven on this port.
- Do not raise the target on VBUS presence alone for an arbitrary cable: keep
  the board ceiling (`0x960`) as a hard cap and require either an enumerated
  host or a proven brick.
- Do not add a re-detection sweep at runtime to "confirm" the source; that is
  the one action that can actually kill the link.

---

## 4. Concrete changes

All line numbers are against `scripts/periph/kernel/apple_j71t_charger.c`
(666 lines) and `apple_tristar_power.c` (559 lines) as built for
`round8e-handoff-bfe22c60a0`.

### 4.1 `apple_j71t_charger.c`

**(a) Presence is not `PRESENT`.** Replace `:201-205`:

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

`-ENODEV` then disappears from this function entirely. A presence that stays
unknown or absent for more than 60 s (for example a gauge that never answers
after the pack is unplugged) must still fail safe: restore the handoff
snapshot instead of continuing at the elevated budget, and log that once.

**(b) Fault only on ownership violations.** Give readback mismatch its own
errno in `j71t_write_checked()` (`:118-130`) — a private
`#define J71T_EREGREADBACK (-EUCLEAN)` is enough — and use it as the only latch
trigger:

```c
	return readback == value ? 0 : J71T_EREGREADBACK;
...
static bool j71t_is_fault(int error)
{
	return error == J71T_EREGREADBACK;
}
```
Replace both classification sites (`:281-283`, `:372-373`) with
`if (j71t_is_fault(ret)) charger->faulted = true;`.

**(c) Bound the latch and retry.** Replace the boolean lifetime with
`unsigned long fault_until;`, and at the top of `j71t_policy_work()`:

```c
	if (charger->faulted && time_after(jiffies, charger->fault_until))
		charger->faulted = false;	/* one guarded retry per minute */
```
Set `fault_until = jiffies + msecs_to_jiffies(60000)` wherever `faulted` is
set. A successful `j71t_enable()` clears both.

**(d) No input-suspend around budget changes.** Add

```c
static int j71t_apply(struct j71t_charger *charger, u8 charge, u8 input)
{
	int ret = j71t_write_checked(charger, J71T_CHARGE_CURRENT, charge);

	if (!ret)
		ret = j71t_write_checked(charger, J71T_INPUT_LIMIT, input);
	return ret;
}
```
and use it in `j71t_enable()` (`:300-310`) and `j71t_restore()` (`:229-241`)
in place of the suspend/write/write/resume sequence. Keep
`j71t_set_suspend(charger, J71T_INPUT_SUSPEND)` only in the "target 0 /
disabled" path.

**(e) Hold through a dip, revoke only on loss.** Replace
`j71t_policy_limit()` (`:170-178`) and the pre-guard block (`:352-354`):

```c
	allow = j71t_source_limit(charger);
	if (allow < 0)
		allow = -EAGAIN;
	/* A suspend or pre-configuration report is a link state, not a source. */
	if (allow < 500000 && charger->active &&
	    time_before(jiffies, charger->hold_until))
		allow = charger->budget_ua;
	if (allow < 500000) {
		if (!charger->hold_until)
			charger->hold_until = jiffies + msecs_to_jiffies(5000);
	} else {
		charger->hold_until = 0;
	}
```
`j71t_revoke_override()` moves to the disconnect/VBUS-loss path and the
explicit `0` write only. `hold_until` is armed once per dip and cleared when
the allowance recovers, so the policy steps down after 5 s instead of
oscillating on the 1 s tick, keeps the owner ceiling, and re-steps up when the
source returns.

**(f) Automatic re-arm.** When a step-down happened but `user_limit_ua` is
still set, retry the owner budget every 30 s (a single guarded
`j71t_enable()` attempt), instead of requiring
`systemctl restart linux-a9-charger-limit`.

Suggested ladder for (e)/(f): `500 → 1000 → 2100 → 2400`, one step per
second, and never more than one step downward per guard trip.
**[I]** iOS sets the value directly; the ramp is a Linux-only safety choice
that bounds the current step the Mac sees.

### 4.2 `apple_tristar_power.c`

**(a) Do not report a suspended bus as a zero source.** In
`apple_tristar_budget()` (`:226-234`) return the *enumerated* budget while
suspended:

```c
	if (budget->connected) {
		*type = POWER_SUPPLY_USB_TYPE_SDP;
		if (budget->host_grant)
			return min(budget->current_ma, 2400U) * 1000;
		if (budget->configured)
			return min(budget->current_ma, 500U) * 1000;
		if (budget->suspended)
			return 500000;	/* link PM, not source capability */
		return min(budget->current_ma, 100U) * 1000;
	}
```
This is the faithful reading of the supplier contract: `CURRENT_MAX` describes
the source, and the source did not change when the host idled the bus.

**(b) Keep everything else.** The `0x01` mux stays behind
`!connected && !host_grant` (`:190`); `0x03` is never written.

**(c) Optional: publish the USB-host class.** If you want the iOS rule rather
than the owner assertion, expose an "Apple USB host" type when `configured &&
!brick && usb-input-limit-max` is known, and let the charger use the board
ceiling. Gate it behind a DT boolean so the default stays "no automatic
entitlement".

### 4.3 Device tree

`arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi`, `charger@180`:

```dts
		apple,usb-input-limit-max-microamp = <2400000>;	/* ADT usb-input-limit-max */
		apple,owner-input-limit-microamp = <2400000>;	/* this dev unit's Mac */
```

The first mirrors `usb-input-limit-max: 0x960` and replaces the hard-coded
`j71t_budgets_ua[]` top entry. The second arms the override at probe, which
removes the boot-ordering race between the charger probe, the udev rule and
`linux-a9-charger-limit.service`. Both must be validated against the existing
500/1000/2100/2400 table and the CBAT codes; an unknown value falls back to
500 mA.

### 4.4 Rootfs

Once (4.3) is in, `linux-a9-charger-limit.service` becomes optional. Keep it
as a manual re-arm tool but drop it from `WantedBy=multi-user.target`, or make
it `Restart=on-failure` with a delay so a transient `EBUSY` is retried rather
than lost.

### 4.5 Validation

Offline, in the existing harnesses:

```sh
python3 -B scripts/periph/test-charging.py          # guard/rollback cases
scripts/periph/check-usb-resume.sh                  # 160 budget assertions
scripts/build-kernel.sh --run periph 'make ARCH=arm64 LLVM=1 \
  drivers/power/supply/apple_j71t_charger.o \
  drivers/power/supply/apple_tristar_power.o'
```

Add cases for: `PRESENT=0` with a readable 3.9 V pack → policy proceeds;
`PRESENT=0` with a failed voltage read → `-EAGAIN`, no latch; I²C `-EIO` from
the supplier → defer, no latch; 5 s of 0 mA allowance → step-down with
`user_limit_ua` still set; recovery → re-step-up; `0xa00` untouched on every
non-zero transition.

On the device, one boot is enough to settle the causal question:

```sh
# 1. which USB event precedes each charger transition
echo 'module dwc2 +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
# 2. charger state at 1 Hz with timestamps
while :; do
  printf '%s %s %s\n' "$(date +%s.%N)" \
    "$(cat /sys/class/power_supply/usb/current_max)" \
    "$(cat /sys/class/power_supply/charger/input_current_limit)"
  sleep 1
done
```

If the `current_max` drop is logged **before** the charger revocation, the
link caused it and 4.1(e)/4.2(a) are the fix. If the charger revokes first
and `current_max` collapses within a second, the current step caused it and
4.1(d) plus the ladder are the fix. Both changes are wanted either way.

---

## 5. Open questions

- **[I]** Whether the `IOAccessoryPowerSourceItemUSBHost` 2400 is a class
  default or set by a restriction/handshake: the allocator
  `FUN_fffffff005de2770` takes the current as an argument and the one call site
  inspected (`FUN_fffffff005df2b80`) passes 0, yet the live item reports 2400,
  and `accessory_apply_source_restrictions` can only lower. The vtable entry at
  `fffffff006de1918` that implements the item's getter is not a defined
  function in the shared analysis, so the constant was not pinned to an
  instruction.
- **[I]** Whether iOS ever writes Tristar `0x03` while the device is
  configured. The detector guards on VBUS/attach state, but that specific
  ordering was not reconstructed.
- **[Unknown]** Whether the Mac port itself current-limits at 2.4 A on this
  cable. iOS's 1704 mA battery current plus system load says it sustains
  roughly 2 A, but no VBUS measurement exists on this board (no
  `function-vbus_voltage` in the dock node), so a droop cannot be observed
  directly.

## 6. Evidence and reproduction

Ghidra outputs for every function cited above are in
`build/periph/ds-charging/ghidra/*.json` (decompiled with
`scripts/ghidra.py decompile_function_by_address address=...`). The live-state
quotes are `build/firmware/private/IOService.txt:7721`, `:7724`, `:13498`,
`:13534`, `:13560`. The log quotes are
`build/omarchy/logs/netconsole-boot8-nvme.log:107943-107965`,
`:124936-124957`, `:69551-69555`. Driver sources are
`scripts/periph/kernel/apple_j71t_charger.c` and `apple_tristar_power.c`,
identical (md5) to the five worktrees inspected in the `linux-a9-src` volume.

Nothing was written to syscfg, NVRAM or the boot firmware; no device was
accessed and nothing was committed.
