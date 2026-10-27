# Round 5: USB host identity and current allowance

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. This was a bounded, static reverse-engineering review. No device
access, register writes, kernel changes, or shared-server restart/save was
performed. Raw Apple code stays under `build/`.

## Finding

**Verified within the paths inspected:** no iOS branch was found that grants
2400 mA merely from high-speed USB enumeration, an inferred Apple host, and
Tristar VBUS presence. Those observations do not establish the missing current
allowance. The current resolver consumes a supplied budget; the 2400 mA dock
constant is a ceiling applied to that budget.

**Verified by the orchestrator:** Round 4 kernel `c7c3eb2d34` received no Apple
current grant and consequently used the ordinary 500 mA source allowance with
zero battery charge current. This is consistent with the inspected iOS
negotiation path, though it does not explain why the Mac omitted the request.

**Inference:** a new “high speed + Apple host + VBUS → 2400” heuristic would be
a Linux policy invention. It would not be a reconstruction of any verified
branch below. No such heuristic is recommended by this report.

## What the relevant iOS code actually does

| Function address | Verified behavior | Consequence |
|---|---|---|
| `fffffff0060e9190` | IOUSBDeviceController setup handler accepts vendor device request 0x40, bmRequestType 0x40 or 0xc0, wLength 0. Signed wIndex supplies extra active mA; signed wValue supplies suspend mA. | An observed request is a concrete additional allowance. |
| `fffffff0060e4074` | Active types 0/5 resolve base plus extra, using `500 + extra` when both are nonzero; clamp to zero and an optional positive maximum. Suspend type 1 uses the suspend allowance; types 2/3/4 return 500 mA. | No host vendor ID, link-speed test, or VBUS capability test promotes the budget here. |
| `fffffff0060e42b0` | Reset establishes the 100 mA base and clears both vendor grant fields. | A previous grant does not survive reset by default. |
| `fffffff0060e4528` | Configuration obtains local configuration MaxPower units, multiplies by 2 for USB2 or 8 for SuperSpeed, and recomputes the active budget. | Speed affects descriptor power units, not host identity or an automatic 2400 mA grant. |
| `fffffff0060e379c` | Gets the local configuration MaxPower value, with defaults 250 units below SuperSpeed and 112 units at SuperSpeed. | The USB2 default is 500 mA; the SuperSpeed default is 896 mA. Neither default is 2400 mA. |
| `fffffff0060e059c` | Reads optional boot argument `usb_max_current` into controller +0x108. | That field is an upper clamp used by the resolver, not evidence of a remote source's capacity. |
| `fffffff006415058`, callback `fffffff00641479c` | Synopsys queues the current update; the callback computes the resolver's mA and calls `function-usb_500_100` with current/type pointers. | The CHRG path carries a computed allowance; it does not add a host-speed grant. |
| `fffffff005e369c4` | USBConnectShim handles CHRG and creates a USBHost source item from its supplied current. | The 2400 mA USBHost item seen under iOS is not synthesized from a 500 mA input to this dispatcher. |

The setup handler is defined in a focused Ghidra project under
`build/periph/round4-source-re/usb-ghidra/`; the shared analysis still has an
incomplete function boundary at that entry. The request branch was checked
against its instructions, including the wValue/wIndex field-order trap.

The configuration MaxPower value is a property of the iPad's device
configuration. `fffffff0060df874` reads local `max-power` and publishes
`MaxPower`; `fffffff0060e64c0` consumes that description. Increasing a device's
own MaxPower description is not proof that a Mac granted a larger allowance.
The review did not assume an artificial upper bound on this internal 32-bit
property; it checked the origin and use of the value instead.

### Source aggregation does not turn a ceiling into a grant

**Verified** low-voltage source item application at `fffffff005de229c` uses
the current carried by the item to select/update the behavior's base current.
For a USBHost item, its constructor `fffffff005de2770` stores the supplied
mA at item +0x28. Its description and family are metadata; they do not select
a new 2400 mA current.

**Verified** Dock9Pin accessory handling at `fffffff005e4e57c` can create a
2400 mA restrictions item through `fffffff005de5070` / `fffffff005de5084`.
The restrictions item's application at `fffffff005de527c` only reduces
maximum current, maximum power and voltage limits. In particular, it updates
the behavior's maximum-current field only when the restriction is smaller.
It does not raise a 500 mA source to 2400 mA.

**Verified** final current calculation at `fffffff005e25904` combines the
selected source/base and current offset, then clips against maximum current
and the power/voltage limit, finally clamping below zero to zero. The saved
iOS behavior has current offset zero. A maximum of 2400 mA therefore cannot
by itself manufacture an extra 1900 mA.

Explicit software APIs also exist for current-base changes. For example,
`fffffff005df2b80` constructs a new current-base or remapped USBHost item;
its callers include `fffffff005e1f98c` and `fffffff005e52430`. These are
explicit policy inputs, not a verified automatic “Apple host” identity test.

## What the device can identify

**Verified in this kernelcache:** the Tristar strict VBUS reader
`fffffff005ff498c` reads register 0x0c and tests the low two bits. The
connection/orientation reader `fffffff005ff4928` tests bits 7 and 6. Neither
returns a current class, an Apple-host vendor identity, or a 2400 mA capability.
Register 0x0f provides the IC revision, not the attached host's identity.

**Protocol inference, consistent with all inspected branches:** ordinary
peripheral enumeration identifies the peripheral to the host. A high-speed
link does not deliver the remote host's USB VID/PID to the peripheral. In
particular, the gadget's own Apple VID 0x05ac describes the gadget; it is not
an observation that the connected Mac has Apple vendor identity. An
Apple-specific setup request may identify host software behavior, but the
examined code still uses the explicit current request's supplied allowance.
No examined branch treats some other Apple-specific request as a 2400 mA grant.

**Verified saved reference state:** IOService line 13561 has D+ and D− both
39 mV, brick ID zero and an SDP item with zero current. Its separate USBHost
item, owned by USBConnectShim, supplies 2400 mA. Thus this Mac's saved iOS
2400 mA state also lacks a usable analog Apple-brick signature. The dump does
not include the original USB control-request tuple or the Mac's decision
about which gadget identities/configurations receive that request.

## Lower-current charging observation

**Verified:** ADT `min-charge-capable-current = 1000` should not be read as a
complete rule saying every source below 1000 mA must have zero battery charge
current. AppleARMPMU source-capability classification at
`fffffff005f11620` uses `_chargeCapableCurrent` in conjunction with current
offset and available current. For the inspected zero-offset case, the
“unsupported” condition tests available current below 300 mA; the
“not charge capable” condition involving the higher threshold also depends
on a positive offset crossing that threshold. Helpers
`fffffff005f16e84` and `fffffff005f11824` obtain the offset and aggregate
available/backoff-limited source current.

This review did **not** reconstruct the full low-source battery-current
allocation, system-load allowance, voltage behavior and thermal policy. It
therefore does not claim iOS programs a particular battery charge current
on a 500 mA USB source.

**Inferred Linux policy selected by the primary agent:** retain the calibrated
nominal 500 mA input allowance (register-equivalent 475 mA on this unit) and
use a bounded 400 mA battery ceiling, D2365 0xa08 code 8. This is a
conservative Linux policy choice, not a verified complete iOS policy match.
Actual battery current remains subject to the input budget and system load.
The owner's explicit sysfs request can restore 2400 mA using the existing
source/temperature/voltage/readback/rollback guards. No additional host
identity heuristic is justified by this RE result.

## Scope, evidence and remaining gap

The negative result is deliberately bounded. The review covered:

- The current resolver, current-request branch, reset, suspend and
  configuration handling in IOUSBDeviceController.
- Direct stores to the current/grant fields in the extracted controller
  instruction range and the local configuration/boot-argument origins.
- Synopsys current dispatch, USBConnectShim CHRG, USBHost item creation,
  restriction application and final source-current aggregation.
- The already mapped Tristar VBUS/revision/orientation readers and the saved
  iOS source items.

It is not a proof over every function in the 90,131-function kernelcache.
It does establish that the proposed passive observations do not justify
changing any of the inspected source-budget decisions to 2400 mA.

New raw results and the bounded search record are in
`build/periph/round5-host-re/`. The direct-store audit is
`controller-direct-field-stores.txt`; it is an immediate-offset instruction
review, not a complete alias-analysis proof. `search-scope.json` records the
review boundary. Earlier exact request and callback records remain under
`build/periph/round4-source-re/`, listed in
[Round 4 source reconstruction](round4-source-recon.md).

The remaining practical gap is the host's decision not to send the extra
current request to the Round 4 Linux gadget. Resolving that requires observed
host/device USB traffic or Mac-side policy evidence. It cannot be filled by
relabeling Tristar VBUS presence or USB link speed as a current grant.
