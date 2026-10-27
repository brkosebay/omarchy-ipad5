# The Mac port: how iOS gets 2.4 A, and why Linux gets 500 mA

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Research note for the `apple-j71t-charger` / DWC2-gadget pair while
the tablet sits on the owner's Mac. Companion to
[ds-charging-policy.md](ds-charging-policy.md) (steady-state policy),
[round4-source-recon.md](round4-source-recon.md) (first reconstruction of the
host grant) and [round4-usb.md](round4-usb.md) (kernel interface).

**Scope.** Offline only: no device access, no kernel edits, no commits, no
syscfg/NVRAM writes. Sources: the shared Ghidra analysis of
`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12` (image base
`fffffff005298000`), the live `build/firmware/private/IODeviceTree.txt` and
`IOService.txt` dumps of this unit, `build/omarchy/logs/netconsole-boot8-nvme.log`,
the driver sources in `scripts/periph/kernel/`, and the `linux-a9-src` volume
(`/src/wt/periph`, `a9/periph`).

Marks: **[V]** verified in code, ADT, live state or log; **[I]** inferred from
consistent evidence, not proven. Every [V] carries the address, file:line or
log:line it comes from.

---

## Bottom line

1. **Two budgets, and only one of them charges.** **[V]** The USB link budget
   is `bMaxPower` (500 mA here) and lives in `IOUSBDeviceFamily`; the budget the
   PMU acts on is a separate accessory number that this Mac resolves to
   2400 mA. Section 1.1.
2. **iOS accepts an explicit "extra current" grant from the host.** **[V]** A
   zero-length vendor request `bmRequestType 0x40, bRequest 0x40` carries two
   signed mA values in `wValue` (suspend) and `wIndex` (extra); iOS stores them
   at controller `+0xfc`/`+0x100` and logs
   `kVendorRequestCurrentLimit received, extra current %d mA, suspend current %d mA`.
   Section 1.2.
3. **The grant is additive to the link budget.** **[V]** For an active USB host
   the resolver returns `500 + extra` when both a base and an extra are known,
   clamps at zero, and caps at `+0x108` (`usb_max_current`, a boot-arg that is
   off by default). Section 1.3.
4. **On this exact Mac iOS resolved 2400 mA.** **[V]** The live dump has
   `IOAccessoryPowerSourceItemUSBHost current limit 2400`,
   `AdapterDetails Current 2400 / Watts 12`, `IsCharging Yes`, `Amperage 1704`
   at 3918 mV — i.e. the pack was taking 1.7 A. With a 500 mA base that means
   **the Mac granted ~1900 mA extra** **[I]**, not that the platform ceiling was
   applied: the boot-arg cap is off, so 2400 could only come from the grant
   arithmetic. Section 1.5.
5. **Linux already implements the receiving end and it has never fired.** **[V]**
   `usb_gadget_handle_current_grant()` (udc/core.c:214) is called first thing in
   `composite_setup()` (composite.c:1778), the board opts in
   (`apple,host-current-budget` in `s8003-j71t-periph.dtsi:123`), and the grant
   would print `Apple host current grant: extra %d mA, suspend %d mA`. There are
   **zero** such lines in `netconsole-boot8-nvme.log` (205k lines, ~20 h, 10
   boots), and `usb/current_max` is 500000. The Mac is not sending the request
   to us.
6. **The charger is not the problem.** **[V]** `input_current_limit` reads back
   the register (`75000 + raw*12500`, `apple_j71t_charger.c:602`), so 2400000
   means `0xa03` really is `0xba`. Nothing in the USB path overrides it: the
   owner override is an override by construction (`j71t_policy_limit()`
   returns `min(max_limit, user_limit)`, `:208`). What actually limits the
   input is the **host port's** allowance **[I]**. Section 2.
7. **Fix: get the grant.** **[V]** The downstream half exists; the missing half
   is the host deciding we are an Apple device worth granting to. The gadget
   presents `05ac:4142` (pongoOS's synthetic PID); iOS presents
   `05ac:12ab`, bcdDevice `0611` **[V, ADT]**. Section 3.
8. **The −300 mA is a 500 mA port, not a register bug.** **[I]** 500 mA × 5 V =
   2.5 W; an idle desktop on this panel is ~3.6 W; −1.1 W ÷ 3.78 V ≈ −300 mA.
   Earlier today at +619 mA the port was delivering ~1.2 A — that was not a
   500 mA port. Check the Mac's sleep state before changing anything
   (Section 4.2): macOS drops port allowances when it sleeps, which is exactly
   what the second field of the vendor request is for.

---

## 1. How iOS draws more than 500 mA from a Mac

### 1.1 The two accounting domains

**[V]** `IOUSBDeviceFamily` keeps a descriptor-derived budget at `+0x104`
(`usb_device_set_configuration_budget`, `fffffff0060e4528`: `2 * bMaxPower` for
USB 2, `8 *` for SuperSpeed), reset to 100 mA
(`usb_device_reset_current_budget`, `fffffff0060e42b0`), zeroed while suspended.
That is the number a USB host is *entitled* to see and it stays 500 mA.

**[V]** The number the charger acts on is produced by the accessory side:
`IOAccessoryUSBConnectShim`'s charger handler `usb_connect_shim_handle_chrg`
(`fffffff005e369c4`) is handed an `ilim` and a `type`, logs
`"%s: chargerFunction: ilim=%dmA type=%x"` (`fffffff005469305`), drives the
platform charger function and publishes
`IOAccessoryPowerSourceItemUSBHost` with `current limit = ilim`. On this unit
`ilim = 2400` **[V]** (`IOService.txt:13560`).

So there are two numbers and the second one is what matters. The rest of this
section is how `ilim` is computed.

### 1.2 The grant request

**[V]** `IOUSBDeviceController::handleRequest` (`fffffff0060e9190`) accepts
vendor requests in both directions (`bmRequestType & 0x7f == 0x40`,
`fffffff0060e91b8`) and dispatches on `bRequest` (`+0x11`). When

| field | value |
|---|---|
| `bmRequestType` | `0x40` (vendor, host→device, device) — `0xc0` takes the same branch |
| `bRequest` | `0x40` (`kVendorRequestCurrentLimit`) |
| `wLength` | `0` |
| `wValue` | suspend current, mA, signed 16-bit |
| `wIndex` | extra active current, mA, signed 16-bit |

it executes `fffffff0060e93f0`: `ldrsh w8,[setup+0x12]` → `str w8,[ctrl+0xfc]`,
`ldrsh w8,[setup+0x14]` → `str w8,[ctrl+0x100]`, then logs through
`fffffff005505fab` (the `extra current %d mA, suspend current %d mA` string) and
acknowledges with a zero-length status stage. There is **no configuration-state
check**: the request is valid before or after `SET_CONFIGURATION`.

**Field order is a trap and it is settled twice.** The store order is wValue →
`+0xfc`, wIndex → `+0x100`, but the log prints `+0x100` as *extra* and `+0xfc`
as *suspend*, and the resolver agrees (1.3). The Linux handler already takes
extra from `wIndex` (`usb_gadget_handle_current_grant()`, udc/core.c:217).

`+0xfc` and `+0x100` are pure host-supplied allowances: reset zeroes both
(`fffffff0060e42b0`), and nothing else in the kernel writes them.

### 1.3 What iOS does with the numbers

**[V]** `usb_device_resolve_current_limit(controller, charger_type)`
(`fffffff0060e4074`), reached from the virtual `getCurrentLimit()` at vtable
`+0x5f8`:

| type | result |
|---|---|
| 0, 5 (unknown / USB host) | `extra + (base or 500)` — `500 + extra` once both are non-zero; clamp ≥ 0; cap at `+0x108` if that is positive |
| 1 (suspended) | `+0xfc` (the suspend value), capped |
| 2, 3, 4 | 500 mA |
| anything else | 0, with an `unrecognized charger type` log |

**[V]** `+0x108` is not a platform constant: `start` fills it from the boot-arg
`usb_max_current` (`fffffff0060e059c`, `_PE_parse_boot_argn`), default 0, i.e.
**no cap**. With `base = 500` and `extra = 1900` the resolver returns 2400
purely from the grant.

**[V]** Reset clears the grant and drops to 100 mA; `SET_CONFIGURATION`
recomputes the base and re-resolves with type 0; bus suspend switches to type 1
(`usb_device_handle_suspend`, `fffffff0060e54d8`). So the grant is *sticky
across configuration changes* but *not across reset* — a gadget rebind on the
Linux side throws it away, which is why the init script's "add ACM after
enumeration" dance matters.

### 1.4 How the number reaches the charger

**[V]** `AppleNubSynopsysOTGDevice::start` (`fffffff0064141f0`) fetches the PMGR
function `function-usb_500_100` and a `_currentLimitThreadcall`
(thread entry `fffffff00641479c`). That thread calls `getCurrentLimit()` and
invokes the platform function with pointers to `{current, type}`
(`fffffff006415058`). The device-tree node that owns the function is the
USB controller:

```
"name"                = "usb-device"
"vendor-id"           = <ac050000>      /* 0x05ac */
"product-id"          = <ab120000>      /* 0x12ab */
"product-version"     = <11060000>      /* bcdDevice 0x0611 */
"function-usb_500_100" = <840000004752484300000000>   /* id 0x84, tag "GRHC" */
```

**[V]** `IODeviceTree.txt:1700-1722`. The shim above requires the platform
function it is handed to be that same object: the property blob must be at least
8 bytes long and carry the tag `"GRHC"` at offset 4 (`fffffff005e36b40`), which
is exactly `function-usb_500_100` and not `function-set_charger` ("grhc"). So
the chain is: vendor request → controller fields → resolver →
`function-usb_500_100` → `IOAccessoryPowerSourceItemUSBHost` → PMU charger.

The charger node has a second, independent function for the PMU itself —
`function-set_charger = <4f00000067726863>` ("grhc") next to
`usb-input-limit-max = <60090000>` (2400) and
`min-charge-capable-current = <e8030000>` (1000) **[V]** (`IODeviceTree.txt:3256-3271`).

### 1.5 What this Mac actually granted

**[V]** Live state of this unit on the owner's Mac
(`IOService.txt:13498`, `:13534`, `:13560`):

```
IOAccessoryPowerSourceItemBrickID      brick id 0, D+ 39 mV, D- 39 mV, current limit 0
IOAccessoryPowerSourceItemUSB_ChargingPort  charging port type "sdp", current limit 0
IOAccessoryPowerSourceItemUSBHost      current limit 2400, adapter "usb host"
AdapterDetails  { Current 2400, Watts 12, PMUConfiguration 2400 }
ChargerConfiguration 2500, IsCharging Yes, Amperage 1704, Voltage 3918
```

**[I]** Read with 1.3: a 500 mA base plus 1900 mA of granted extra is the only
arithmetic in that function that yields 2400 with the boot-arg cap disabled.
The 1704 mA into the pack (≈ 6.7 W) plus system load means the port was really
delivering roughly 1.8–2.0 A at 5 V, not 500 mA. **That is the thing Linux is
missing, and it is a host-side allowance, not a device-side number.**

This also corrects [ds-charging-policy.md](ds-charging-policy.md) §1.3, which
read the same 2400 as the board ceiling being applied to a "usb host". The
board ceiling (`usb-input-limit-max`) only *caps* what the shim publishes; the
value itself is the USB resolver's, and the resolver can only reach 2400 here
because 1900 mA arrived in `wIndex`.

**[I]** Why the host allowance follows the grant: the host port computes and
publishes `kUSBWakePortCurrentLimit` / `kUSBSleepPortCurrentLimit`
(`fffffff005f6f924` logs `kUSBWakePortCurrentLimit %d mA`) from the bus-current
allocation machinery whose classes are present in this kernelcache
(`AppleUSBHostBusCurrentPool`, `AppleUSBHostBusCurrentAllocator`,
`kUSBHostReturnNoPower`). A port that allocated only `bMaxPower` has no reason
to allow more than that; the grant is what re-allocates it.

### 1.6 The two paths that do *not* explain it

- **BrickID / Tristar.** **[V]** The Mac's D+/D− sit at 39 mV (its 15 kΩ
  pull-downs): brick ID 0, limit 0. The 9-entry table (`0x5476a30`, mirrored in
  `apple_tristar_brick_current()`) only helps for a resistive Apple charger.
  No brick, no current from this path.
- **USB-C current advertisement.** **[V]** J71t is a Lightning board: the ADT
  lists `IOAccessoryUSBTypeCCurrentDetect` with zero instances, and the
  controller node has no CC function. With a USB-C→Lightning cable the Mac's CC
  sees only the cable's own Rd (passive, no e-marker, no PD), so the Type-C
  contract is "default USB" = 500 mA and nothing on the device side can change
  it. The only way up is the software port limit macOS raises for Apple devices
  **[I]**.

### 1.7 The identity the Mac sees

**[V]** iOS enumerates as `05ac:12ab`, bcdDevice `0611`, strings `"Apple Inc."` /
`"iPad"` (ADT above). **[V]** The Linux gadget enumerates as `05ac:4142`
(`scripts/boot/autoboot.sh:49-50` documents the Mac's view: DFU `05ac:1227`,
recovery `05ac:1281`, pongoOS `05ac:4141`, **iOS `05ac:12ab`, the Linux gadget
`05ac:4142`**), `bcdDevice` at the kernel default
(`get_default_bcdDevice()` = the Linux version in BCD, i.e. `0x0612` on 6.12),
`bMaxPower` 500 (`CONFIG_USB_GADGET_VBUS_DRAW=500`,
`configs/hoolock-16k.config:3565`).

**[I]** macOS is not broadcasting the grant to every device — it did not send it
to `05ac:4142` across ten boots of that capture — so the trigger is the Apple
product identity (PID and/or bcdDevice), not the VID alone. This is the single
unverified link in the chain and it is cheap to test.

---

## 2. What Linux does today

### 2.1 The SDP path

**[V]** With the gadget configured and no grant,
`apple_tristar_budget()` (`apple_tristar_power.c:220`) returns
`min(budget.current_ma, 500) * 1000`; suspended returns 500000; connected but
unconfigured returns 100 mA. `budget.current_ma` is `usb_gadget_vbus_draw()`,
i.e. 500. That is the `usb current_max 500000` / `usb_type [SDP]` the owner
sees, and it is correct behaviour for an ungranted link.

### 2.2 The override really writes 2400 mA

**[V]** `apple,owner-input-limit-microamp = <2400000>` in the board DTS sets
`user_limit_ua` at probe (`apple_j71t_charger.c:745`). `j71t_policy_limit()`
(`:208`) returns `min(max_limit_ua, user_limit_ua ?: source_ua)` — the owner
value **replaces** the 500 mA allowance, it is not clamped by it. `j71t_enable()`
(`:345`) then writes `0xa08 = 0x32` (2500 mA) and `0xa03 = 0xba` (2400 mA) with
readback verification, and `input_current_limit` reads the register back
(`:602`). The boot8 log confirms the sequence:
`owner override budget 2400 mA: input 0xba, battery limit 2500 mA`.

**So no, the USB-side 500 mA does not win.** Nothing in the driver caps the
override, and the PMU register carries 2400. The 500 mA that "wins" is not in
our software at all — it is the Mac's port allocation **[I]**.

### 2.3 The grant path is armed and has never fired

**[V]** `usb_gadget_handle_current_grant()` (udc/core.c:214) validates exactly
the tuple from 1.2, stores `extra`/`suspend`, logs
`Apple host current grant: extra %d mA, suspend %d mA` and notifies;
`composite_setup()` calls it before anything else (composite.c:1778);
`apple_tristar_budget()` would then return `min(current_ma, 2400)` (`:230`); the
board opts in at `s8003-j71t-periph.dtsi:123`.

**[V]** `grep -c "Apple host current grant" netconsole-boot8-nvme.log` = 0, and
`usb/current_max` never left 500000. The request is not reaching us.

### 2.4 Why the pack discharges with the charger "online"

**[I]** `0xa03` is a *ceiling on what the charger may pull*, not a supply. The
input power is `min(port allowance, 0xa03) × VBUS`. With the port at 500 mA that
is 2.5 W against ~3.6 W of idle-desktop load, i.e. −1.1 W ≈ −300 mA at 3.78 V —
exactly the measurement. The same register value produced +619 mA earlier when
the port was delivering ~1.2 A. Neither `0xa08` (2500 mA battery limit) nor
`0xa0c` (cap, 3150 mA) is the constraint; `0xa00` bit 1 would zero the input
outright and is worth ruling out (Section 4.1).

### 2.5 Register and node reference

| Item | Where | Meaning |
|---|---|---|
| `0xa03` | D2365 input current limit | `code = ((mA*8-600) & 0xfff8)/100`; `0xba` = 2400, `0x20` = 475 |
| `0xa08` | battery charge current | 50 mA steps; `0x32` = 2500 |
| `0xa00` bit 1 | input suspend | only for a true disable / target < 75 mA |
| `0xa0c[5:0]` | charge-current cap | latched at attach; guard rejects `< charge_code` |
| `0x18a` | status | bits 1:0 charging, 2 termination, 4 timeout |
| `usb-device` node | ADT `:1700-1722` | `05ac:12ab:0611`, `function-usb_500_100` |
| `charger` node | ADT `:3256-3271` | `usb-input-limit-max` 2400, `function-set_charger` |

---

## 3. The concrete fix

Everything on the receiving side exists. The work is to make the Mac treat this
gadget the way it treats an iPad, and to stop hanging up on the rest of the
Apple protocol.

### 3.1 Present the iPad's identity **[I]**

Set in the configfs gadget before bind (HoolockRD's `init_functions.sh`, outside
this repo; the repo's `scripts/omarchy/initramfs/init` only rewrites strings):

```
idVendor  = 0x05ac      (unchanged)
idProduct = 0x12ab      (was 0x4142)
bcdDevice = 0x0611      (was the kernel default, 0x0612 on 6.12)
bMaxPower = 500 mA      (unchanged — match iOS, do not inflate it)
```

Rationale: the grant is the only mechanism in the Apple stack that raises a Mac
port above 500 mA, it is PID/bcdDevice-conditional in practice (no grant in ten
boots to `4142`), and `12ab`/`0611` is the exact pair the Mac grants 1900 mA to.
Keep `bMaxPower` at 500 so the arithmetic in 1.3 stays `500 + extra`; inflating
it would change the base and, if macOS clamps at the port maximum, change the
grant too.

### 3.2 Do not stall the other Apple vendor requests **[I]**

macOS drives an Apple device through several vendor requests; the device side
recognises at least `0x50`, `0x52` and `0xff` besides `0x40`
(`fffffff0060e93a0` and `fffffff0060e962c`, plus
`kVendorRequestSelectExtendedFunction`). Our composite returns `-EOPNOTSUPP`
for all of them, which EP0-stalls. If macOS sequences "identify → select
extended function → grant current", the stall in the middle aborts the grant.

Cheap, low-risk change in `usb_gadget_handle_current_grant()`
(udc/core.c:214): for `bmRequestType 0x40/0xc0` with `wLength == 0` and
`bRequest` in `{0x50, 0x52, 0xff}`, log once and return 0 (accept, zero-length
status) instead of `-EOPNOTSUPP`. Do not implement the functions; just do not
refuse them. Keep the current `0x40` path exactly as it is.

### 3.3 If the identity alone is not enough **[I]**

Then compare descriptors rather than guessing. On the Mac,
`ioreg -p IOUSB -l -w0` for the iPad and for the Linux gadget gives
`bcdUSB`, `bDeviceClass/SubClass/Protocol`, `bMaxPacketSize0`, `bNumConfigurations`,
`bMaxPower` and the string indices side by side. Candidates in order of
likelihood: `bcdUSB` (composite sets `0x0201` only when the UDC is LPM-capable,
composite.c:1839), a BOS/device-capability descriptor, `iSerialNumber` format,
`iProduct`/`iManufacturer` strings. Each is one configfs/kernel knob, and each
is testable in a single boot.

### 3.4 Limits and policy

- **Cap the total at 2400 mA** — the board's `usb-input-limit-max`. The udc
  core already clamps to 2400 and rejects `|extra| > 2400` (udc/core.c:239).
- **Grant only raises; it never bypasses the guards.** Temperature, `0xa0c` and
  termination stay authoritative.
- **Keep the owner override for the ungranted case**, but treat it as a
  measurement tool, not a policy: at 2400 against a 500 mA port the charger
  pulls whatever the port allows and the port's over-current protection is the
  only thing standing between us and a link drop (the root filesystem is on NBD
  over this cable). If the grant never arrives, the honest steady state is a
  budget the port can actually sustain, not 2400.
- **Never revoke and re-arm to "retry" a grant.** The grant is delivered by the
  host; re-binding the gadget destroys it (reset clears it, 1.3).

---

## 4. Device test plan

### 4.1 Read-only first (no writes, no rebind)

```sh
# what the PMU actually holds (regmap debugfs; CONFIG_DEBUG_FS + REGMAP are on)
d=/sys/kernel/debug/regmap; ls $d
cat $d/*pmic@3c/registers | grep -E '^(a00|a03|a08|a0c|18a|1f)'
# driver's own view (0xa03 read back, 0xa08 read back)
cat /sys/class/power_supply/charger/{online,usb_type,status,input_current_limit,constant_charge_current}
cat /sys/class/power_supply/usb/{online,usb_type,current_max}
cat /sys/class/power_supply/battery/{current_now,voltage_now,power_now,status,capacity}
# link state, and whether any grant was ever seen
cat /sys/class/udc/usbdev/state
dmesg | grep -E 'Apple host current grant|owner override budget|Apple brick signature'
```

Record `0xa03`, `0xa00`, the pack current and `usbdev/state` in the same
sample. `0xa03 = 0xba` with `0xa00[1] = 0` and a small pack current is the
signature of a port shortfall (2.4); `0xa00[1] = 1` or `0xa03 ≠ 0xba` is a
driver-side problem instead.

There is no VBUS ADC on this board (`function-vbus_voltage` is absent from the
dock node), so VBUS has to come from an inline USB power meter if it is needed
at all.

### 4.2 Decide between the two candidate causes

| Cause | Test | Expected |
|---|---|---|
| Mac asleep / port allowance reduced | wake the Mac, keep the same boot, sample again | pack current returns to ~+619 mA; `current_max` still 500000; no grant log |
| No Apple grant | none yet — see 4.3 | stays at −300 mA while the Mac is awake |

This is the first thing to run: it costs one minute and it decides whether the
fix in Section 3 is even needed for the owner's daily use.

### 4.3 After the identity change

1. Boot, let the gadget settle, then
   `dmesg | grep 'Apple host current grant'`. A line with
   `extra 1900 mA, suspend N mA` confirms the whole chain in one shot.
2. `cat /sys/class/power_supply/usb/current_max` → expect 2400000.
3. `cat /sys/class/power_supply/charger/input_current_limit` → 2400000, and the
   pack current positive.
4. On the Mac, `ioreg -p IOUSB -l -w0` should show `idProduct 0x12ab`; note
   whether the device now appears as an iPad (cosmetic, but it proves macOS
   took the Apple path).
5. Repeat with the Mac asleep and with a bus suspend/resume to learn the
   `suspend` field's real value.
6. Only then decide whether the owner override should stay armed.

### 4.4 What would falsify this

- Grant arrives with `extra 1900` but the pack current does not improve → the
  port allowance is not tied to the grant on this Mac, and iOS must be getting
  its current some other way (then re-open 1.5).
- Mac-awake test alone restores +619 mA → the grant is irrelevant for this port
  and the correct policy is a host-state-aware budget, not a negotiation.

---

## 5. macOS sender: who sends 0x40/0x40, and why `4142` never gets it

**This section corrects §1.5 and §4.2.** The 2400 mA iOS resolved on this Mac is
*not* a host grant of "500 + 1900 extra". 2400 is a **static capability macOS
looks up by VID:PID**; the vendor request's `extra` field is *derived from* that
number afterwards (`2400 − 500 = 1900`). The request is only ever sent to a
device that has the capability entry — which is exactly why ten boots to
`05ac:4142` produced no grant.

Sources: `AppleUSBHostCompositeDevice` (v1.2), `AppleUSBHostiOSDevice` (v1.0),
`IOUSBHostFamily` (v1.2), `AppleUSBHostMergeProperties` (v1.2) and their
`Info.plist`s, all from the running macOS 27.0 (26A428, Mac16,7 / M4 Pro)
kernelcache
`/System/Volumes/Preboot/…/System/Library/Caches/com.apple.kernelcaches/kernelcache`
(`ipsw kernel dec`, `ipsw kernel extract`, `llvm-objdump`, Ghidra headless), plus
a live `ioreg -l -w0` of the same machine. Addresses are unslid kernelcache
addresses; the kexts keep local symbols, so the function names below are theirs.

### 5.1 The one and only sender **[V]**

Pattern-scanning all 55 `*usb*` / `*Accessory*` kexts for a `movz wN,#0x4040`
(the `bmRequestType 0x40, bRequest 0x40` pair) yields exactly one site:

`AppleUSBHostCompositeDevice::negotiatePower(IOTimerEventSource*)`,
`fffffe000b5daf98`:

| what | where | value |
|---|---|---|
| setup packet | `fffffe000b5db408` `mov w10,#0x4040; strh w10,[sp,#0x40]` | `bmRequestType 0x40`, `bRequest 0x40` |
| `wValue` | `[sp,#0x42]` ← the `uint32` returned by provider `vtable+0x950` | suspend mA |
| `wIndex` | `[sp,#0x44]` ← the `uint16` from the same call `−500` (`ldrh` + `sub w9,w9,#0x1f4`, `fffffe000b5db404`) | **allocation − 500** |
| `wLength` | `[sp,#0x46]` ← `wzr` | 0 |
| transport | provider `vtable+0xb58`, timeout `0x1f4` | `DeviceRequest` |

On success it writes `kUSBConfigurationCurrentOverride = allocation` on the
device (`vtable+0x2a0`); on failure it **removes** that property and, if the
provider query failed entirely, sends the explicit revoke `wValue 0,
wIndex −500` (`0xfe0c` at `fffffe000b5db3f0`).

So the tuple is field-for-field the one iOS parses in §1.2 — `wValue` suspend,
`wIndex` extra — and "extra" is literally `allocation − 500`, not a number the
host picks out of thin air.

### 5.2 The gate is a property, not a descriptor **[V]**

`AppleUSBHostCompositeDevice::start()` (`fffffe000b5daa58`) creates two timers,
`configure` (`this+0xb0`) and `negotiatePower` (`this+0xb8`), then picks **one**
of them:

```c
prop = provider->getProperty("UsbPowerSinkCapability");   /* fffffe000b5dace0 */
arm( (prop is an OSNumber) ? negotiatePowerTimer : configureTimer );
```

A device without that property **never runs `negotiatePower`** — no capability ⇒
no vendor request, ever, no matter what its descriptors say.

`UsbPowerSinkCapability` is not derived from `bMaxPower` and is not in any
descriptor. It is merged onto the `IOUSBHostDevice` from a static table in
`AppleUSBHostMergeProperties` (`IOKitPersonalities` → `IOProviderMergeProperties`),
keyed **only** on `idVendor` + `idProduct` (one iPhone entry adds a
`bcdDeviceRange`). Excerpt for VID `0x05ac`:

| VID:PID | mA | note |
|---|---|---|
| 05ac:**0x12ab** | 2400 | the iPad; also matches `AppleUSBHostiOSDevice` |
| 05ac:0x12a8 | 1000 / 2400 | iPhone, `bcdDeviceRange` 0…1791 / 1792…65535 |
| 05ac:0x1397 | **3000** | no other 05ac personality in `/System/Library/Extensions` |
| 05ac:0x1014, 0x1015 | 2400 | idem |
| 05ac:0x1390/0x1395/0x1396/0x1398/0x1399/0x1404 | 2400 | idem |
| 05ac:0x029a/0x029b/0x029c/0x029f | 2400 | old iPod/iPhone PIDs, idem |
| 05ac:0x0501/0x0502, 0x026e/0x026f, 0x0451/0x0454/0x0455 | 3000 | idem |
| non-Apple: 291a:0195 (Anker), 046d:d101 (Logitech) | 3000 / 2400 | the table is not Apple-only |
| **05ac:0x4142** | — | **absent** |

The value then flows: `IOUSBHostDevice::setProperty` special-cases the key
(`fffffe000b47eedc`) → `AppleUSBHostBusCurrentAllocator::allocateBusCurrent`
(`fffffe000b4b4694`) aggregates the request with the port's other clients and
publishes `UsbPowerSinkAllocation = max(wake,sleep)` on the device
(`fffffe000b4b5620`) and `kUSBBusCurrentAllocation` on the hub/port. The pool is
the root port's `kUSBWakePortCurrentLimit` / `kUSBSleepPortCurrentLimit`
(`initBusCurrent`, `fffffe000b464940`), which on this Mac are **3000 / 3000 mA**
(`ioreg`, `AppleT8132USBXHCI` / `usb-drd1-port-hs`).

`UsbPowerSinkAllocation = 500` for `05ac:4142` therefore means "no table entry,
fall back to `bMaxPower`". It is not macOS judging our descriptors.

### 5.3 Why `0x12ab` stopped enumerating **[V code / I branch]**

`0x12ab` is not merely a table key — it also matches a device driver.
`AppleUSBHostiOSDevice.kext` has `idVendor 0x05ac`, `idProductArray
{0x1290,0x12a0,0x12b0}`, `idProductMask 0xfff0`, `IOProbeScore 10000`, and no
class/interface constraints: `0x12ab & 0xfff0 == 0x12a0` → match. (`0x4142 &
0xfff0 == 0x4140`, and every PID recommended in 5.6, do **not** match.)

That driver owns configuration selection for iOS devices:

- `probe()` (`fffffe000b5e1438`) requires the device's `kUSBVendorString` to
  contain `"Apple"`, then opens the device and sends **`0xC0/0x45`**
  (`kAppleUSBVendorRequestPreferredConfig`), `wLength 4`, 500 ms
  (`fffffe000b5e1570`). A failed request ⇒ `probe` returns 0.
- `getPreferredConfiguration()` (`fffffe000b5e1d48`) parses that 4-byte reply
  (byte 1 = device configuration preference, byte 2 = host preference, byte 3 =
  extended function) and its return value is the `bConfigurationValue` the stack
  will use. No usable answer ⇒ 0.
- `start()` (`fffffe000b5e1898`) only arms a 1 s retry timer.

**[Superseded by §5.7]** The live test showed the same failure for `0x1397`,
which this driver does not match, so the `AppleUSBHostiOSDevice` path was never
reached: the abort is the port's TRM identification restriction. The description
above still holds for what this driver would do *if* a `0x12ax` device were ever
identified.

A gadget that EP0-stalls `0xC0/0x45` gives this driver no preferred
configuration, so the device is left at configuration 0 — unconfigured, 100 mA.
**[I]** Which of the two branches actually fired on the failed boot (probe reject
vs. config 0) cannot be told offline; both follow from the same missing reply,
and both produce the observed symptoms. `AppleUSBEthernetHost2` also carries the
same `0xfff0` mask, but its personality requires an interface with class 255 /
subclass 253, so it does not bind our ECM interfaces (2/6/0, 2/10/0).

Default configuration choice in the absence of a driver override
(`AppleUSBHostCompositeDevice::getPreferredConfiguration`,
`fffffe000b5dc624`; `AppleUSBConfigurationDescriptorOrderedSet::orderFunction`,
`fffffe000b5dd8b0`): `kUSBPreferredRecoveryConfiguration` >
`kUSBPreferredConfiguration` (device or driver property) > **highest
`bMaxPower`**, tie-broken by **highest `bConfigurationValue`**.

### 5.4 The descriptor set to present **[V]**

What is on the wire today already configures, binds and passes traffic, so this
is a delta, not a redesign (`ioreg` of the live gadget, `05ac:4142`,
`bcdDevice 0x0703`, `bcdUSB 0x0200`, `USBSpeed 3`):

| field | value | why |
|---|---|---|
| `idVendor` | `0x05ac` | table is keyed on Apple VID |
| `idProduct` | see 5.6 | **the only field that matters for current** |
| `bcdDevice` | free (keep `0x0611` or `0x0703`) | only `0x12a8` has a range check |
| `bDeviceClass/SubClass/Protocol` | `0/0/0` | matches `AppleUSBHostCompositeDevice` 0/0; keep as is |
| `bNumConfigurations` | 1 | the ordered set copes; a second config only matters via 5.3 ordering |
| `bConfigurationValue` | 1 | fine |
| `bMaxPower` | **500** | the default allocation when no capability entry exists; the `−500` in the request is hardcoded, so a larger `bMaxPower` only raises what the config must be afforded and risks the deconfigure path |
| interfaces | ECM (2/6/0 + 2/10/0) + ACM (2/2/1 + 2/10/0) | unchanged; ECM already binds `AppleUserECM` |
| `iManufacturer` | must **not** contain `"Apple"` | only relevant if a `0x12ax` PID is ever tried; it is the first gate in `AppleUSBHostiOSDevice::probe` |
| `iProduct`, `iSerialNumber` | free | no policy keys off them |

There is **no descriptor** that produces `UsbPowerSinkCapability`. `bMaxPower`
does not substitute for it (§5.2), and a SuperSpeed configuration with 8 mA
units is not available on this HS-only DWC2 gadget.

### 5.5 Current without the vendor request, on an A-cable **[V]/[I]**

- **BC1.2 / D+/D−: not a route.** A host port does not advertise a charging
  signature; brick-ID detection (`IOAccessoryManager`'s `brick id` /
  `IOAccessoryPowerSourceItemBrickLimit` strings) is the *device* reading a
  *charger*, and the iPad itself reports `charging port type "sdp"` with
  `current limit 0` on the same cable (§1.5) while drawing 1.7 A. The extra
  current is not a D+/D− contract. **[V]**
- **No CC/PD either**: USB-A Lightning cable through a C→A adapter, so the
  Type-C contract is default-USB and there is no e-marker to talk to. **[V]**
- **`RequestExtraPower` is not device-callable.** `IOUSBHostDeviceUserClient`
  exposes `RequestExtraPower` / `ReturnExtraPower` / `GetExtraPowerAllocated`
  (`fffffe000b42a0c0`, `…a514`, `…a788`) — that is the in-kernel/DriverKit API a
  *host-side* client uses to bid for bus current; it is what
  `allocateBusCurrent` implements. A USB device cannot invoke it. **[V]**
- **iAP authentication** exists as a separate gate (`Authenticated`,
  `iAPAuthenticator`, `kUSBFailedRequestedPower` in `IOUSBHostDevice::setProperty`)
  but it applies to Apple *accessories*, not to the capability table. **[V]**
- **The earlier `+619 mA` is not evidence of >500 mA.** 500 mA × 5 V = 2.5 W; at
  ~3.9 V pack voltage and ~92 % charger efficiency that is ≈ 590 mA into the
  pack with the panel off. `−300 mA` at the same 500 mA input means ~3.6 W of
  load. The two readings differ by *system load*, not by port allowance, so §4.2
  ("Mac asleep / allowance reduced") is probably a red herring. **[I]**

Net: on this link the only lever is the software allocation of §5.2 — and that
lever does not need the `0x40/0x40` request to move. The request is a
*notification* to the device (iOS uses it to raise its own policy); the current
that flows is what the Mac's port allocator allows, which is set from
`UsbPowerSinkCapability` at enumeration.

### 5.6 Recommended gadget configuration, ranked

All of them keep 5.4's descriptor set; only `idProduct` changes, before bind.

1. **`idProduct = 0x1397`** (capability **3000**). No other `05ac` personality in
   `/System/Library/Extensions` matches it (checked all 157 `idVendor 1452`
   personalities across the 918 kext/plugin `Info.plist`s), it is outside
   `AppleUSBHostiOSDevice`'s `0xfff0` mask, and 3000 mA equals this port's
   `kUSBWakePortCurrentLimit`. Expected `UsbPowerSinkAllocation = 3000`.
2. **`idProduct = 0x1014`** (2400). Same properties, and 2400 is the number iOS
   itself resolves, so it is the closest match to the reference behaviour.
3. **`idProduct = 0x029a`** (2400). Same in-kernel properties, but it is a
   classic iPod/iPhone PID: userspace (`MobileDevice`/`usbmuxd`) may probe it.
   Use only if 1–2 fail.
4. **`idProduct = 0x12ab` + implement the Apple protocol** — iManufacturer
   containing `"Apple"`, plus a `0xC0/0x45` handler replying 4 bytes with
   byte 1 = `bConfigurationValue` (e.g. `00 01 01 00`), plus accepting
   `0x50/0x52/0xff` without stalling. Highest fidelity (2400 + a real grant +
   the `Apple host current grant` log line) but it is the only option that
   needs new device-side protocol work; it is also the option that failed
   today. Do it last, and only if 1–3 show an allocation but no current.

configfs sketch (HoolockRD's `init_functions.sh`, before `bind`):

```sh
G=/sys/kernel/config/usb_gadget/g1
echo 0x05ac > $G/idVendor
echo 0x1397 > $G/idProduct        # rank 1; 0x1014 for rank 2
echo 0x0611 > $G/bcdDevice
echo 0x0200 > $G/bcdUSB
echo 500    > $G/configs/c.1/MaxPower
# strings: keep iManufacturer free of "Apple"
```

**Success indicator, on the Mac.** Per device:

```sh
ioreg -p IOUSB -l -w0 -n "Omarchy on iPad (linux-a9)" |
  grep -E 'UsbPowerSink(Capability|Allocation)|idProduct'
# name-independent sweep:
ioreg -p IOUSB -l -w0 |
  grep -E '"(USB Product Name|idProduct|UsbPowerSinkCapability|UsbPowerSinkAllocation)"'
```

Rank 1 expects `"UsbPowerSinkCapability" = 3000` (the merged table value) and
`"UsbPowerSinkAllocation" = 3000`; the current state shows only
`UsbPowerSinkAllocation = 500` and no capability property. On the tablet the
`Apple host current grant` line may still be absent (it depends on
`AppleUSBHostCompositeDevice` winning the device over the `com.apple.driver.usb.cdc`
composite personality — today the CDC nub is what sits under our device, and
`AppleUSBHostCompositeDevice` does not start for it); that is acceptable,
because the port allowance, not the notification, is what the charger sees.

Falsifiers: allocation stays 500 with a rank-1 PID → the merge personality did
not fire (check for an `AppleUSBHostMergeProperties` nub and the capability
property first); allocation rises to 2400/3000 but pack current does not → the
port does not deliver its allocation and §1.5 must be reopened.

### 5.7 Live test, same day: the blocker is TRM, not the capability table

**This supersedes §5.3 and adds a precondition to §5.6.** The owner tried
`idProduct = 0x1397` on the live gadget (UDC unbind → set PID → bind, cable
untouched, `bcdDevice 0x0703`, empty manufacturer string, one ACM+ECM config,
`MaxPower 500`). Result identical to `0x12ab`: `IOUSBHostDevice` node present
with only `idProduct`/`bcdDevice`/`kUSBAddress`, `!registered, !matched` for
>30 s, gadget reporting "not attached". Reverting to `0x4142` registers and
configures in ~2 s. `0x1397` matches no `05ac` personality other than the merge
entry, so `AppleUSBHostiOSDevice` (§5.3) was not involved: **the failure is
generic to any identity this port has not seen before.**

**[V] The gate.** `AppleUSBHostPort::createDevice` (`fffffe000b4600d4`) calls
vtable slot `+0xa40`, which resolves (via `__ZTV16AppleUSBHostPort + 0x10`) to
**`AppleUSBHostPort::isIdentificationRestricted()`** (`fffffe000b46879c`). It
reads an `IOPortTransportState` at `port+0x3a8`: `isTRMSupported()` then flag
`[+0x145]` (identification) / `[+0x146]` (transport). When restricted the device
is messaged `0xe000510c` and returned **without** being started, which is
exactly the observed half-built node. The completion block
(`fffffe000b461bdc`) repeats the check after identity is known: it requires
`UsbDeviceSignature`, publishes its hash with `IOPortTransportState::setHash()`,
and returns `0xe00002d8` ("device functionality blocked by transport
restrictions") if the port is still restricted — so enumeration aborts before
`registerService()` and the port is disabled (hence "not attached" on Linux).

**[V] The policy, from this Mac's live registry** (`ioreg`, `IOPortTransportStateUSB3`,
`Port-USB-C@2`):

```
"TRM_Profile" = 2            "TRM_ProfileDescription" = "Ask for New Accessories"
"TRM_State"   = 2            "TRM_StateDescription"   = "Limited"
"TRM_TransportRestricted" = Yes   "TRM_IdentificationRestricted" = No
"AuthorizationRequired" = Yes     "AuthorizationStatus" = 1  "Unauthorized"
"TRM_DeviceLocked" = Yes          "TRM_GracePeriodReasonDescription" = "Accessory Detached"
"HashStatus" = 0                  "HashStatusDescription" = "Not Set"
```

`HashStatus` is the TRM's per-port cache keyed by the hash of
`UsbDeviceSignature`, and that signature is `idVendor | idProduct | serial`
(the working node carries `<ac05 4241 "REDACTED" …>`). A new PID is a new
signature, i.e. a new accessory under the "Ask for New Accessories" profile,
so identification is held until it is authorized. `0x4142` has been authorized
on this port for months, which is why it alone registers. **[I]** Whether the
prompt was suppressed by `TRM_DeviceLocked`, by no UI session, or by policy is
not decidable offline; the userspace half is `accessoryaccessd`
(`AccessoryAccess.framework`, `AaCore::Usb::UsbDeviceIdentity`,
`user_did_assign_usb_device`, entitlement
`com.apple.developer.accessory-access.usb`).

**Consequences for 5.6.** No descriptor change can make a table PID register —
the authorization is keyed on the PID. The order of operations must be:
set `idProduct` with the UDC unbound **and the cable out**, bind, then plug in
(authorization runs on a port connect event, not on a UDC rebind under a live
port); unlock the Mac, approve the prompt, or set *System Settings → Privacy &
Security → Allow accessories to connect → Always*. Only then does the
capability merge of §5.2 happen and `usbpowerd` (/usr/libexec/usbpowerd,
launched by `IOPropertyExistsMatch = UsbPowerSinkCapability`) raise the
allocation. Check `TRM_IdentificationRestricted = No` plus a non-`Unauthorized`
`AuthorizationStatus` before reading anything into `UsbPowerSinkAllocation`.

**Control experiment** that would have caught this sooner: a PID that is *not*
in the capability table (e.g. `0x4143`) fails to register identically, so the
table was never the discriminator.

**Logging.** Kernel kext messages are not in the unified log on this system
(`log show --last 4h --predicate 'process == "kernel"'`: 12 lines, none USB), so
no `log show` predicate will show the IOUSBHostFamily TRM lines; they are
`_IOLog`/masked os_log only (`sudo dmesg`, or the `AppleUSBCommon` debug mask).
The userland half is reachable:
`log show --last 15m --info --debug --predicate 'process == "accessoryaccessd" OR eventMessage CONTAINS[c] "TRM"'`.
The authoritative readout is the `ioreg` TRM block above.

---

## 6. Open questions

- ~~**[I]** Whether macOS gates the grant on `idProduct`, `bcdDevice`, or the
  full descriptor set.~~ **Answered in §5.2**: `idProduct` only, via the static
  `UsbPowerSinkCapability` table; `bcdDevice` only for `0x12a8`; descriptors not
  at all. The `0x12ab` failure is the separate `AppleUSBHostiOSDevice` path
  (§5.3).
- **[I]** The exact granted pair on this port (`extra 1900, suspend ?`). The
  arithmetic pins the extra value, not the suspend value; 4.3 step 1 prints both.
- **[Unknown]** Whether the Mac raises the *hardware* port limit on grant or
  merely stops enforcing `bMaxPower`. Both look identical from the device; an
  inline meter during 4.3 distinguishes them.
- **[Unknown]** Whether `usb_max_current` is set on retail iOS. It is unset in
  this kernelcache's default path, so 2400 cannot be a cap artefact — but a
  boot-arg on a shipping device would change the reading of 1.5.

## 7. Evidence and reproduction

Ghidra outputs for the functions cited here are reproducible with
`scripts/ghidra.py decompile_function_by_address address=...`:
`fffffff0060e9190` (vendor request dispatch), `fffffff0060e4074` (resolver),
`fffffff0060e42b0` (reset), `fffffff0060e4528` (configuration budget),
`fffffff0060e54d8` (suspend), `fffffff0060e059c` (`usb_max_current`),
`fffffff0064141f0` (OTG start), `fffffff005e369c4` (accessory shim).
Live state is `build/firmware/private/IOService.txt:13498`, `:13534`, `:13560`;
the ADT is `build/firmware/private/IODeviceTree.txt:1700-1722` (USB device node)
and `:3256-3271` (charger node). Log quotes are
`build/omarchy/logs/netconsole-boot8-nvme.log` (`owner override budget`,
and the absence of `Apple host current grant`). Linux sources:
`scripts/periph/kernel/apple_j71t_charger.c`, `apple_tristar_power.c`,
`drivers/usb/gadget/udc/core.c`, `drivers/usb/gadget/composite.c` in the
`linux-a9-src` volume.

Nothing was written to syscfg, NVRAM or the boot firmware; no device was
accessed and nothing was committed.
