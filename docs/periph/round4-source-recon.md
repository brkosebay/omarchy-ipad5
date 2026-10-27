# Round 4: automatic J71t USB source budgets

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Static reconstruction only; this agent never accessed the device.
Claims marked **verified** come from this kernelcache or the saved live
IOService dump. Proposed Linux policy is marked **inferred**. All raw Apple
code and private data remain under `build/`.

## The Mac's 2400 mA comes from USB negotiation

**Verified, decisive live evidence:** `build/firmware/private/IOService.txt`
line 13561 lists the contributing Power Source Items separately:

- BrickID item: D+ 39 mV, D− 39 mV, brick ID 0, current limit 0.
- ChargingPort item: `sdp`, primary detection voltage 39 mV, current limit 0.
- USBHost item owned by `IOAccessoryUSBConnectShim`: current limit 2400 mA.
- Dock9Pin restriction item: maximum current 2400 mA.

The resolved behavior at line 13563 is `usb host`, USBData, 2400 mA.
Therefore analog Apple brick detection alone will not recover this Mac's
2400 mA budget. The host grant enters through the USB controller's CHRG
platform-function path. VBUS presence alone grants no elevated current.

### Exact host control request

**Verified** at IOUSBDeviceController setup handler `fffffff0060e9190`
(independent Ghidra function definition):

| USB setup field | Accepted value / meaning |
|---|---|
| bmRequestType | Exactly 0x40 or 0xc0: vendor, device recipient |
| bRequest | 0x40 (`kVendorRequestCurrentLimit`) |
| wLength | 0 |
| wValue | Signed 16-bit **suspend** current, mA; stored at controller +0xfc |
| wIndex | Signed 16-bit **extra active** current, mA; stored at +0x100 |

The handler updates current with type 0 and acknowledges the zero-length
request. No configuration-state check precedes this branch in iOS.
**Field-order trap:** the setup fields are stored as wValue then wIndex,
but the log passes +0x100 as “extra current” and +0xfc as “suspend current”.
The instruction argument order and current resolver agree. Reversing the
meanings would apply the wrong budget.

**Verified** resolver `fffffff0060e4074`:

- Base USB budget is at +0x104; optional positive maximum is at +0x108.
- For active type 0 or 5: when both base and extra are nonzero, candidate
  current is `500 + extra`; otherwise it is `base + extra`. Clamp below zero
  to zero, then cap to +0x108 if that maximum is positive.
- For suspend type 1: use +0xfc, capped by a positive maximum. This branch
  does not add a base budget.
- Types 2, 3 and 4 return 500 mA.

USB reset `fffffff0060e42b0` sets the base to 100 mA and clears both host
request fields. Configuration handler `fffffff0060e4528` sets base current
from the selected descriptor: `2 * bMaxPower` for USB2, `8 * bMaxPower` for
SuperSpeed, then recomputes type 0. Suspend handler `fffffff0060e54d8` selects
type 1.

**Verified delivery chain:** Synopsys override `fffffff006415058` queues a
thread, whose entry is `fffffff00641479c`. That thread computes the current
through virtual getter +0x5f8 and invokes the ADT-resolved
`function-usb_500_100` with pointers to current and type. J71t's ADT targets
dock `CHRG(0)`. IOAccessoryUSBConnectShim's dispatcher
`fffffff005e369c4` recognizes CHRG; for ordinary host type it creates an
`IOAccessoryPowerSourceItemUSBHost` with that supplied current. The source
aggregation then applies the J71t dock/platform maximum of 2400 mA.

**Inferred Linux implementation contract:** recognize this exact request in
EP0, cache the bounded signed budgets and defer power-supply work out of
atomic setup context. Elevate current only after an observed host grant;
cap at 2400 mA. Revoke the grant on USB reset/disconnect, apply the suspend
budget on suspend, and restore active budget on resume. Negative values
must not wrap to a large unsigned limit. iOS permits reductions through
negative extra values; Linux can reject unsupported ranges or clamp safely
while documenting the difference. The device test must log the actual
request tuple: the saved IOService dump proves the resolved 2400 mA grant,
but does not retain that tuple.

**Unknown until device test:** whether this Mac sends the request to the
Linux gadget's Apple VID/product ID and current descriptor layout. The
HoolockRD gadget uses VID 0x05ac and PID 0x4142 per the implementation agent's
inspection; this makes the request plausible, not verified. An unobserved
grant must leave the ordinary USB budget in effect.

## Apple resistor signature classification

**Verified** IOAccessoryUSBPowerSourceDetect's `determineUSBCharger` at
`fffffff005de8328` obtains USB_DP and USB_DM pin voltages. For an Apple
resistor signature, `fffffff005de44c4` uses these integer mV thresholds:

1. If either pin is below 1500 mV, brick ID is zero.
2. Otherwise, independently bucket each pin as low (1500–2199 mV), medium
   (2200–2879 mV), or high (2880 mV and above).
3. Brick ID is `1 + 3 * dp_bucket + dm_bucket`, where buckets are 0, 1, 2.

`fffffff005de450c` initializes the BrickID item and indexes the nine-entry
current table at `fffffff005476a30`. The table is independently decoded from
the extracted kext's `__TEXT,__const` section:

| Brick ID | D+ bucket | D− bucket | iOS nominal mA | J71t bounded mA |
|---|---|---|---:|---:|
| 0 | Either below 1500 | — | 0 | Ordinary USB fallback policy |
| 1 | Low | Low | 500 | 500 |
| 2 | Low | Medium | 1000 | 1000 |
| 3 | Low | High | 1000 | 1000 |
| 4 | Medium | Low | 2100 | 2100 |
| 5 | Medium | Medium | 2400 | 2400 |
| 6 | Medium | High | 3000 | 2400 |
| 7 | High | Low | 1000 | 1000 |
| 8 | High | Medium | 1000 | 1000 |
| 9 | High | High | 1000 | 1000 |

**Inferred Linux policy:** keep ID 0 distinguishable from a verified ID 1
although both may result in a 500 mA fallback. Validate signed voltages and
ADC success before using this unsigned iOS classifier; reject out-of-range
or failed measurements. The J71t cap comes from ADT and the live restriction
item, not the generic 3000 mA entry. An ordinary configured unknown USB host
gets the requested 500 mA Linux fallback; iOS's intermediate unknown source
items above can be zero before USB enumeration/grant adds a host item.

## Exact temporary Tristar DP/DM mux

**Verified** AppleTriStarBuiltIn's voltage getter +0x5e8 is a trampoline at
`fffffff005fe52bc` to its IC's +0x630 operation. CBTL1610's entry
`fffffff005fef718` delegates to the inherited
`fffffff005ff5a08` pin-voltage operation when allowed by controller state.

That operation uses cached baseline DX_CTRL, register 0x01. Its routing
preconditions and monitor settings are:

| Existing route | Preconditions | USB_DP selector 1 | USB_DM selector 2 |
|---|---|---|---|
| USB1 | `(saved & 0x07) == 1` | `(saved & 0xf0) \| 0x0c` | `(saved & 0xf0) \| 0x0d` |
| USB2 | USB1 test false, `(saved & 0x70) == 0x10` | `(saved & 0x0f) \| 0xc0` | `(saved & 0x0f) \| 0xd0` |

An unsupported baseline returns an error; it is not replaced with a guessed
USB configuration. The operation:

1. Serializes the measurement.
2. Writes the selected one-byte DX_CTRL value to register 0x01.
3. Reads register 0x01 back and reports a mismatch.
4. Invokes optional analog-front-end callbacks, then sleeps 5 ms.
5. Calls BuiltIn's `readBrickID` at `fffffff005febe00`, which invokes this
   unit's `function-brick_id_voltage` (`D2365 adcV(1, 0x10000)`).
6. Restores the exact cached baseline DX_CTRL even if the ADC call failed.

For the two selectors above there are **no DCP_CTRL register 0x03 writes**.
Selectors 4 and 5 are separate BC charging-port tests; they also alter
DCP_CTRL and are outside the minimal Apple resistor-signature procedure.

The optional callbacks before ADC are BuiltIn +0x880 →
`fffffff005fed080` (EIS AFE enable, argument 0) and +0x888 →
`fffffff005fed0e8` (LDCM bypass, argument 1). They resolve function objects at
+0x1178 and +0x1180. **Verified:** this J71t ADT has none of those functions
or `supports-eis-afe`, so both are absent/no-ops on this unit.

**Inferred Linux bounds:** measure only while USB is unconfigured, with a
single mutex covering save, selection, ADC and restoration. Save the actual
register byte instead of reconstructing it; require the expected route,
REV 0x8a and strict VBUS before and after the pair. A readback mismatch,
restore failure, ADC error, source change or invalid orientation must
invalidate the classification. Avoid repeating monitor writes during active
USB transfers. A stable repeated classification/debounce requirement is a
Linux safety policy; no passive Tristar “2400 mA ready” bit was identified.
The iOS flow reserves the relevant pins while detecting.

Existing verified presence facts remain applicable: register 0x0c low two
bits both set means strict VBUS present; bit 7 indicates a physical
connection, with bit 6 selecting orientation 1/2. These bits identify cable
state, not source current entitlement. Do not consume interrupt register
0x0d for this polling measurement.

## ADC boundary and evidence

The D2365 trigger/completion implementation is being reconstructed by the
primary peripherals agent, to avoid duplicate work. This report establishes
the source classifier and mux contract, not an independent ADC-completion
implementation. Raw functions already located are preserved here for that
agent: `6bc0e58` writes channel 1 to 0x4000; `6bc0e8c` reads two bytes from
0x4001; `6bc1704` selects the channel-1 voltage conversion. A fresh-completion
check is required before consuming those bytes.

All raw results, focused Ghidra projects, extracted kexts and instruction
cross-checks are under `build/periph/round4-source-re/`. Inputs are the same
kernelcache SHA256 recorded in `docs/periph/charging-recon.md`. The relevant
new source evidence is:

- `brick-id-from-voltages.json`, `brick-item-init.json`.
- `ths-read-pin-adc.json`, `tristar-ghidra/fffffff005fed080.json`, and
  `tristar-ghidra/fffffff005fed0e8.json`.
- `usb-ghidra/fffffff0060e9190.json`, `usb-current-resolve.json`,
  `usb-reset.json`, `usb-suspend.json`, `usb-set-config.json`.
- `host-discovery.json` (the USBConnectShim CHRG dispatcher).
- `usb-chrg-callback.json` plus the matching Synopsys instruction listing;
  shared Ghidra merges this callback into its caller, so the instruction
  entry `fffffff00641479c` is authoritative for that boundary.

Ten understood functions were renamed/commented in shared Ghidra; the setup
handler and current table received comments. The annotation results are in
`build/periph/round4-source-re/annotations.log`. No kernel files were changed
by this RE workstream.

## Implemented policy clarification

The final Linux implementation is deliberately stricter than the candidate
contract above: suspend revokes elevated grants and resume requires a fresh
grant; the cached iOS suspend allowance is logged but not applied. Persistent
bus-connected state, including observed EP0 SETUP, excludes enumerating and
sleeping data hosts from analog classification. There is one classification
attempt per connection after the initial quiescence delay, rather than repeated
debouncing. ADC failure poisons uncertain conversions, and exact route
restoration is retried. See [the final report](round4.md) for remaining timing
assumptions and [the USB interface](round4-usb.md) for its lifetime contract.
