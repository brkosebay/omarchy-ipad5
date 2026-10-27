# usb-host5: what iOS programs for `configureUSBMode(2)`, and what Linux was missing

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-27. Offline reverse engineering and implementation. No device access.

**Sources.** The 2026-10-27 iOS capture of the camera adapter on this unit
([ios-adapter-capture-20261027.md](ios-adapter-capture-20261027.md); raw ioreg trees in
`build/periph/ios-adapter-capture/20261027T095244/`). The shared Ghidra analysis of the
iOS 16.7.10 J71t kernelcache (functions below were renamed and commented, and the analysis
was saved). Undefined functions were disassembled with capstone from
`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`. The Linux side comes from
the `a9/kexec-irqfix-usb4` tree and the register dumps of the usb-host,
usb-host3 and usb-host4 trials (`logs/usb-host*-20261026/`).

**Tags.** **[V]** verified: the code, the capture or a Linux hardware dump shows exactly
this. **[I]** inferred: consistent with the evidence, not proven.

---

## 1. Answer in one page

1. **Mode 2 is requested from user space.** `IOAccessoryManagerUserClient` selector 1
   is `configureUSBMode(scalar[0])` **[V]**. No kernel path produces 2. The port's
   `getUSBMode` returns 0, 3, 4 or 5. The `IapOverUsbHid` 0x51 request and CHRG type 5
   both call `configureUSBMode(3)` **[V]**. So an iOS daemon sends 2 about 1 s after the
   ID is read **[I]**. Linux has to make the same decision itself.
2. **Mode 2 means "a USB device is on the port, no authentication".** Mode 2 maps to
   ConnectType 4 "Device", then to cable type `USBDevice`, then to arbitrator publish
   **mask 5** **[V]**. Mode 3 differs only in `AuthenticationRequired` and the cable name
   `USBDeviceNeedsAuthentication`, which also gives mask 5 **[V]**. The USB hardware ends
   up the same for both.
3. **Mask 5 keeps the device controller.** `publish-criteria & mask`: `usb-device`
   (criteria 3) stays published and EHCI and OHCI (criteria 4) are added **[V]**. The
   capture shows this: `AppleNubSynopsysOTGDevice` and all its gadget interfaces are
   still present beside `usb-ehci0` while the hub is enumerated **[V]**. ds-usb-host.md
   §3.3 ("strict either/or") is wrong for this mask.
4. **The PHY is a vote between clients.** The OTG device client votes for device mode,
   and the EHCI/OHCI port client votes for host mode. Both votes are active in mode 2,
   so the PHY runs at power level 3 with host mode on **[V code; capture-consistent]**.
5. **The register that Linux never wrote.** The PHY power-up (`setPowerLevel >= 2`)
   sets **`USB_CTL+0x00` bit 1**, and iOS keeps it set while the PHY is powered **[V]**.
   iOS host mode therefore has `USB_CTL+0 = 0x3` **[I, from the code]**. Every Linux
   host-mode dump shows `0x1`, and `0x0` in device mode **[V]**. All other PHY values
   already match iOS: CTL `0x10`, SIG bit 1, host `cfg0`/`cfg1`, trim 0, reset pulse.
   Whether bit 1 is what makes the port detect the hub (Linux saw `sig=se0`) is **[I]**.
   It is the only PHY-side difference left.
6. **iOS's device-mode enable is PHY SIG (`+0x04`) bit 1, not `USB_CTL+4`** **[V]**. This
   corrects ds-usb-host.md §3.4. phy-apple-ausb already sets that bit
   (`VBUSDET_FORCE_EN`).
7. **Charging.** iOS reports `USBPower Yes` and a 2100 mA input limit while it is the
   host **[V capture]**. Linux's supplier forced `online = 0` in host mode **[V source]**.
   Linux's "tristar" gate (`0x05` bit 0) zeroes the CBTL1610 VBUS bits (`0x0c` reads `a3`
   without it, `a0` with it) **[V usb-host4 dumps]**, so that bit must stay clear.
8. **usb-host5** (branch `a9/usb-host5`, four commits on irqfix-usb4):
   - `USB_CTL` bit 1 is set before the host select.
   - A kernel accessory policy handles 11 f0: GetID, power handshake, host role with no
     revert, kept until unplug, then device mode again.
   - The supply reports an ACA source at 500 mA, then 2100 mA after the handshake, also
     in host mode.
   - The root-port budget is a parameter (500 mA).
   - `lightning_state` sysfs gives the readout.

---

## 2. The chain, step by step

### 2.1 Who asks for mode 2

| Caller of `configureUSBMode` | Mode | Evidence |
|---|---|---|
| `IOAccessoryPortUSB` vendor request 0x51 (iAP-over-HID) → 50 ms → worker | 3 | `6c3d5c4` [V], ds-usb-host.md §3.1 |
| `usb_connect_shim_handle_chrg` (`5e369c4`), CHRG type 5 | 3 | [V] |
| `IOAccessoryManagerUserClient_sel1_configureUSBMode` (`5e1f900`) | `scalar[0]` | [V] |
| `IOAccessoryDock9Pin_classify_digitalid_usbfield` (`5e4f064`, the port's `getUSBMode`) | 0, 3, 4 or 5 | [V] |

The user client dispatch table is at `fffffff006dee840`. It holds 36 entries of 24 bytes
(function pointer, scalar-in count, struct-in size, scalar-out count, struct-out size), and
`externalMethod` at `5e1f834` indexes it **[V]**. Selector 0 sets the power mode and logs
`IOAccessoryManagerUserClient::%s: PowerModeOff` **[V]**. Selector 1 requires one scalar
input **[V]**.

For 11 f0 (`USB=2`, `DB=0`), `getUSBMode` returns 0 (device mode) **[V]**. That matches
capture snapshot 025: `USBModeType 0`, `USB2 DataRole None`. One snapshot later (about
1.5 s) it is mode 2 with `DataRole Host` **[V]**.

### 2.2 `configureUSBModeGated` (`5df2228`) → `updateState` (`5dee61c`)

- Stores the mode at `+0x25c` and sets the "configured" flag `+0x258` **[V]**.
- For the USB2 and AIDBus transports, modes 0, 1 and 5 mean active with DataRole 1
  (device). Modes **2 and 3 mean active with DataRole 2 (host)**. Mode 4 and anything
  out of range mean inactive **[V]**.
- `AuthenticationRequired = (mode == 3)` **[V]**.
- Then `IOAccessoryManager_updateState` → `IOAccessoryManager_updateUSBConnectType`
  (`5df3004`) **[V]**.

### 2.3 Mode → ConnectType (`5df3004`)

| mode | ConnectType | name (table `6de4b80`) |
|---|---|---|
| 2 | 4 | **Device** |
| 3 | 5 | DeviceAuthRequired |
| 5 | 2 | Host |
| 0, 1, 4 | from the dock's power-source items (the Mac cable: 2 "Host") | |

All **[V]**. The capture's 026 snapshot shows `ConnectType 4 "Device"` and
`CurrentLimitBase 2100` **[V]**.

### 2.4 ConnectType → cable type (`usb_connect_shim_publishCableType`, `5e37094`)

| ConnectType | AppleUSBCableType (USB3 transport inactive / active) |
|---|---|
| 2 | USBHost / USB3Host |
| 3 | USBHostAltConfig / USB3HostAltConfig |
| 4 | **USBDevice** / USB3Device |
| 5 | USBDeviceNeedsAuthentication / USB3DeviceNeedsAuthentication |
| other, USB2 inactive, or restricted mode | Detached |

The type is published as the resource `AppleUSBCableType` **[V]**. The symbols are built by
`59d95d8` from the strings at `52acd47..` **[V]**.

### 2.5 Cable type → publish mask (`AppleEmbeddedUSBArbitrator_publishMaskForCableType`, `60776c4`)

| cable type | mask | J71t result (criteria: device 3, EHCI/OHCI 4) |
|---|---|---|
| USBHost, USBHostAltConfig | 3 | device only |
| **USBDevice, USBDeviceNeedsAuthentication** | **5** | **device kept (3 & 5 = 1) + EHCI/OHCI (4 & 5 = 4)** |
| USB3Device* | 0x21 | — |
| USB3Host* | 0x41 | — |
| Detached / unknown | 1 | device only |

`applyPublishMask` (`6077958`) publishes the host nubs before the cable message when
`mask & 6`. `updatePublishedNubs` (`60770f8`) terminates a nub only when
`criteria & mask == 0` **[V]**. Capture 026 shows `usb-device@100000` with its full
gadget tree beside `usb-ehci0` and `usb-ohci0` **[V]**.

### 2.6 The EHCI port powers on for a "device" cable

`AppleUSBEHCIRemovablePort_cableChange` (`67d5abc`): for USBDevice and its three
variants, "cable change: powering on port" (power state 3). For any other type it powers
the port off **[V]**. Powering the port on starts the port's PHY service client
(type 0) **[V]**.

### 2.7 The PHY is a vote

- `AppleUSBPhyServiceClient_init_typeToMask` (`6075358`): client type → mask via table
  `54ed440`. Type 0 (host port) → **0x4**, type 1 (OTG device) → **0x8**, −1 (power
  assertion) → 0x10, 2..4 → 0x2 **[V]**.
- `AppleUSBPhy_enablePhyPower_votes` (`6074848`), with `assignClientToVoteArrays`
  (`6074a54`): mask bit *i* puts the client into vote array *i*. The **power level is the
  highest index that has a client**, passed to `setPowerLevel` (slot `0x548`). Array 2
  going 0↔1 calls `enableHostMode(1/0)`. Array 3 going 0↔1 calls `enableDeviceMode(1/0)`
  **[V]**.
- In mode 2 the OTG device client (array 3, plus array 4 for its power assertion) and
  the EHCI port client (array 2) are both active. So the level is 3 or 4, host mode is
  on and device mode stays on **[V code; matches the published nubs]**.

### 2.8 The register writes (`AppleS5L8960XUSBPhy`, base `+0xd0` = PHY `0x20c000030`, `+0xd8` = USB_CTL `0x20e0d8000`)

`start` (`6cbf1f0`) maps reg[0] as `_registerMap` and reg[1] as `_usbCtlMap`, then does
**`USB_CTL+0 &= ~0x2`** **[V]**.

`setPowerLevel` (`6cbf890`, not a Ghidra function; disassembled; jump tables at
`5949fb8`/`5949fbd`) **[V]**:

| step | writes |
|---|---|
| 0 → 1 | none |
| 1 → 2 | clock enables for `clock-mask` 1; **`USB_CTL+0 \|= 0x2`**; PHY `+0x1c = 0`; CTL `\|= 0x3`; `cfg0`/`cfg1` = device words; CTL `&= ~0x36`; 20 µs; CTL `&= ~0x1`; 1 ms; jitter fix if errata bit 0 |
| 2 → 3 | CTL `\|= 0x10`; CTL `&= ~0x8` |
| 3 → 4 | none |
| 3 → 2 | CTL `\|= 0x8`; CTL `&= ~0x10` |
| 2 → 1 | PHY `+0x1c = 0`; CTL `= 0x3f`; **`USB_CTL+0 &= ~0x2`**; clocks off |

`enableHostMode` (`6cbfd58`): the clock gate is held. On enable it does `USB_CTL+0 |=
0x1`, writes `cfg0-host` `0x37373b03` and `cfg1-host` `0x00020c04`, sets trim to 0,
does CTL `|= 1`, waits 20 µs, CTL `&= ~1`, then waits 1 ms. The reset pulse runs only
at level > 1 **[V]**. On disable it does `USB_CTL+0 &= ~0x1` and writes the device
words **[V]**.

`enableDeviceMode` (`6cbfc40`): **PHY `+0x04` (SIG) bit 1** is set or cleared under the
clock gate **[V]**. It does not touch `USB_CTL+4`.

Resulting iOS state in mode 2 **[I, derived from the code above]**: `USB_CTL+0 = 0x3`,
PHY CTL `0x10`, SIG bit 1 set, `cfg0 = 0x37373b03`, `cfg1 = 0x00020c04`, trim 0.

### 2.9 Tristar and accessory power

- No function on the mode-2 path (sections 2.2 to 2.8) writes the CBTL1610 **[V for
  those functions]**. Some virtual calls from `updateState` were not followed, so
  "mode 2 writes no Tristar register at all" is **[I]**.
- `IOAccessoryActivePowerMode` goes from 1 to **4** at identification (snapshot 025,
  before mode 2) **[V capture]**. `AppleTriStarBuiltIn_getPowerModeCurrentLimit`
  (`5feae5c`) maps mode 4 to `acc_sw-current-limit`, 5 to `acc_bm3-…`, and 2/3 to
  `acc_pwr-…` **[V]**. The port's `setPowerMode` (vtable slot `0x668` = `5feb22c`)
  drives the dock functions `function-acc_pwr` (`+0x1108`), `acc_sw_en` (`+0x1118`) and
  `acc_sleep_pwr` (`+0x1120`), which are read in `readDockCapabilities` **[V]**. The exact
  order inside `setPowerMode` was not fully decoded **[I]**. On J71t these are D2365 LDO5
  `pmuL(5)` and its switch `pmuL(0x80000005)` (ADT), which is the Linux "pmu" gate.
- `CurrentLimitBase 2100` appears with mode 2 (026). `USBCurrentLimit 2100`,
  `ChargingVoltage 5000` and `USBPower Yes` follow `PowerHandshake Yes` (028) **[V
  capture]**. Which function picks 2100 for this accessory was not located **[I]**.
- Tristar `0x05` bit 0 (`setVBUSSink`): Linux trials show `0x0c = 0xa3` with gates
  none/pmu and `0xa0` with the tristar gate (`0x05 = 0x01`) **[V,
  `logs/usb-host4-20261026/*/host-*.txt`]**. iOS charges in mode 2, so bit 0 is clear
  there **[I]**.

---

## 3. Linux (usb-host4 host path) vs iOS mode 2, register by register

| Item | iOS mode 2 | Linux before usb-host5 (hardware dumps) | usb-host5 |
|---|---|---|---|
| `USB_CTL+0` bit 0 (host select) | 1 [V] | 1 [V] | 1 |
| **`USB_CTL+0` bit 1 (PHY on)** | **1, whenever the PHY is powered [V]** | **0, never written [V]** | **1 in host mode** (`usb_ctl_phy_on`, default on) |
| `USB_CTL+4` | never written [V] | 0, "device enable" knob is misnamed [V] | unchanged, comment corrected |
| PHY CTL `+0x00` | `0x10` at level 3 [I] | `0x10` [V] | same |
| PHY SIG `+0x04` bit 1 | 1, device vote kept [V] | 1, `VBUSDET_FORCE_EN` [V] (`0x1c103`/`0x1c003`) | same |
| `cfg0`/`cfg1` | `0x37373b03`/`0x00020c04` [V] | same [V] | same |
| trim `+0x1c` | written 0 [V] | written 0; reads `0x08000000` [V] | same |
| Order | EHCI port powered, then `enableHostMode` [V] | PHY host first, then EHCI created [V] | same as Linux [I: see §5] |
| Device controller | published, clocks on (`clock-mask` 2) [V] | DWC2 bound, gadget unbound, `DCTL` SftDiscon [V] | same as Linux [I: see §5] |
| EHCI/OHCI | published [V] | created by the role switch [V] | same |
| Tristar `0x05` bit 0 | clear [I] | set by gate `tristar`, kills VBUS bits [V] | refused for the accessory path |
| Tristar `0x01`/`0x02` mux | not written on this path [I] | `01=00`, not written [V] | not written |
| Accessory power (acc_sw / LDO5) | on, ActivePowerMode 4 [V] | opt-in gate `pmu` [V] | on for the accessory (`accessory_gates=pmu`) |
| Mode entry | user space `configureUSBMode(2)`, ~1 s after the ID [V/I] | manual `arm`, or waiting for iAP 0x51 that never comes [V] | kernel policy on ID 11 f0 |
| Power handshake | done ~4 s after mode 2 [V] | manual, debugfs, gated [V] | automatic, before host (two tries; host anyway) |
| Input power in host mode | USBPower Yes, 2100 mA [V] | supplier offline in host mode [V] | ACA, 500 → 2100 mA after the handshake |
| Root-port budget | `AAPL,power-supply` 0x32 [V] | 100 mA hard-coded [V] | `apple_port_budget_ma`, default 500 |

---

## 4. What Linux was missing

1. **`USB_CTL+0` bit 1** **[V missing]**. This is the one remaining PHY-side
   register difference in host mode. That it explains the empty port (`PORTSC 0x501000`,
   `sig=se0`, no hub pull-up seen) is **[I]**. usb-host5 sets it before the host select,
   in the iOS order (power-up bit 1, then `enableHostMode`), and clears it after the host
   select on the way back.
2. **A direct host path.** The adapter is a hub waiting for a host. The iAP responder
   waited for a SETUP that never comes **[V capture + usb-host4 trial]**.
3. **Charging in host mode.** The supplier dropped `online` for any host role
   **[V]**.
4. **The tristar gate is harmful** for charging: it clears the VBUS status **[V]**.
5. **Enumeration accounting.** A 100 mA root budget can refuse a bus-powered hub's or
   a drive's configuration. That looks like an electrical failure but is accounting **[I risk]**.

Deliberate deviations from iOS, kept for safety:
- **[I]** iOS sets `USB_CTL` bit 1 in device mode as well. usb-host5 sets it only in host
  mode, because device mode already works without it.
- **[I]** iOS keeps the device-mode vote and powers the EHCI port before
  `enableHostMode`. Linux unbinds the gadget and switches the PHY before creating EHCI.
  In register terms the PHY ends up the same (§3). If the hub still does not attach,
  these are the next candidates (§6).

---

## 5. What usb-host5 does

Kernel branch **`a9/usb-host5`**, four commits on `209a5a55e3` (irqfix-usb4). It contains
`a9/kexecfix` (`a412bc8f04`, an ancestor) and `a9/kexec-irqfix` (`7dcb18c411`,
`e3ad1b3a5c`, cherry-picked as `8f3730511e`, `209a5a55e3`; `git cherry` patch-identical).
The patches are in `patches/usbhost5/`.

1. `usb: roles: apple-s8000: iOS mode-2 PHY programming, accessory host role`. This
   commit:
   - sets `USB_CTL` bit 1 for host mode (`usb_ctl_phy_on=1`);
   - adds an accessory-claimed host role with no revert (`apple_s8000_usb_role_claim()`);
   - adds `accessory_gates` (default `pmu`; `tristar` refused);
   - adds sysfs `accessory` and shows the bits in debugfs `regs`.
2. `power: supply: apple_tristar: camera adapter to USB host, as iOS does`. This adds
   the policy in `apple_tristar_auto.h`, which is pure and covered by the harness:
   - GetID without reorient, 300 ms after a plug edge, then every 2 s with no USB link
     or every 10 s while a host uses the gadget. It is quiet: only changes are logged.
   - For 11 f0: the power handshake (two tries), then the claimed host role.
   - Host role: the ID is re-checked every 5 s and on a plug edge. Three empty `0x0c`
     reads, or a different ID, return the port to device mode and the gadget rebinds.
   - A role set by the user makes the policy back off until the next unplug.
   - Supply: ACA, online whenever VBUS is present, 500 mA, then `auto_input_ua`
     (2100000) after the handshake.
   - `auto_host=0` turns the policy off.
   - `/sys/bus/i2c/devices/0-001a/lightning_state` (world-readable) and debugfs
     `apple-tristar/auto_log` give the readout.
3. `power: supply: apple_j71t_charger: accept the camera adapter as a source`. ACA is
   eligible, so the charger ramps 500 → 1000 → 2100 mA, one step per second.
4. `usb: host: ehci/ohci-platform: J71t root-port budget as a parameter`
   (`apple_port_budget_ma`, default 500, read at each host entry).

The manual debugfs identify, handshake and recover interfaces and the `arm`/`listen`
helpers are unchanged. `allow_power_handshake` still gates only the debugfs handshake.
None of the changes touches syscfg, NVRAM or boot storage.

---

## 6. If the hub still does not attach (next discriminators, no rebuild)

All of these are runtime knobs. Each one applies at the next host entry (unplug and plug
the adapter again):

1. Check that the fix landed. In host mode, `regs` must show `usb_ctl: +00=00000003`,
   `tristar … 05=00 0c=a3` and `pmu ldo5: 0310=01 … 2603=01`. If it does not, nothing
   else can be judged.
2. `usb_ctl_phy_on=0`: an A/B test of bit 1 alone. If the hub only enumerates with
   bit 1 set, bit 1 is confirmed as the cause.
3. `accessory_gates=none` vs `pmu`: an A/B test of accessory power.
4. `apple_port_budget_ma=100`: reproduces the old budget, to tell a refused
   configuration apart from no attach.
5. Next entry, if all of the above fail **[I]**: keep the DWC2 device side powered
   (iOS keeps the device vote), and create EHCI before `enableHostMode` (iOS powers the
   EHCI port first).

---

## 7. Corrections to earlier documents

- ds-usb-host.md §3.4: `enableDeviceMode` toggles **PHY SIG (`+0x04`) bit 1**, not
  `USB_CTL + 4`. The open question "must `USB_CTL+4` be cleared" is moot. The question
  that matters is `USB_CTL+0` bit 1, which the PHY power-up sets **[V]**.
- ds-usb-host.md §3.3 and §1.3: publication is not strictly either/or. For a USB device
  on the port (mask 5) the device nub stays **[V code + capture]**.
- ds-usb-host.md §3.1, ds-lightning-host-idbus.md §7: the camera adapter takes neither
  the 0x51 path nor `configureUSBMode(3)`. iOS uses mode 2 from user space (§2.1).
- usb-host.md, the "tristar" VBUS gate: on this board it removes the VBUS status
  (`0x0c` `a3` → `a0`) **[V]**. Do not use it with a powered adapter.

---

## 8. Evidence index (renamed in the shared Ghidra project)

| Address | Name |
|---|---|
| `5df2228` | `IOAccessoryManager_configureUSBModeGated` |
| `5dee61c` | `IOAccessoryManager_updateState` |
| `5df3004` | `IOAccessoryManager_updateUSBConnectType` (comment: mode → type table) |
| `5e1f834` / `6dee840` | user-client `externalMethod` / dispatch table |
| `5e1f900` | `IOAccessoryManagerUserClient_sel1_configureUSBMode` |
| `5e4f064` | `IOAccessoryDock9Pin_classify_digitalid_usbfield` (`getUSBMode`) |
| `5e37094` | `usb_connect_shim_publishCableType` |
| `60776c4` | `AppleEmbeddedUSBArbitrator_publishMaskForCableType` |
| `6077958` | `AppleEmbeddedUSBArbitrator_applyPublishMask` |
| `60770f8` | `AppleEmbeddedUSBArbitrator_updatePublishedNubs` |
| `67d5abc` | `AppleUSBEHCIRemovablePort_cableChange` |
| `6075358` | `AppleUSBPhyServiceClient_init_typeToMask` (table `54ed440`) |
| `60754e4` | `AppleUSBPhyServiceClient_requestPhyServiceLevel` |
| `6074848`, `6074a54` | `AppleUSBPhy_enablePhyPower_votes`, `…assignClientToVoteArrays` |
| `6cbf1f0` | `AppleS5L8960XUSBPhy_start` (`USB_CTL &= ~2`) |
| `6cbf890` | `setPowerLevel` (disassembly comment; not a Ghidra function) |
| `6cbfc40`, `6cbfd58` | `enableDeviceMode` (SIG bit 1), `enableHostMode` |
| `5feae5c` | `AppleTriStarBuiltIn_getPowerModeCurrentLimit` |
| `5feb22c` | `AppleTriStarBuiltIn` `setPowerMode` (vtable `6e52150 + 0x668`; undefined in Ghidra) |

Capture files: `NNN-tree.txt` (025 → 026 shows EHCI/OHCI added and the device nub kept),
`NNN-transportusb2.txt` (DataRole None → Host), `logs/ios-adapter-20261027/*-tristar-props.txt`.
Linux dumps: `logs/usb-host4-20261026/2026*Z-b332c721/host-*.txt` (`usb_ctl: +00=00000001`,
`tristar 05/0c`).

---

## 9. usb-host5 on hardware (2026-10-27 11:17) and usb-host5b

Logs: `logs/usb-host5-20261027/`, run `build/usb-host5/device-logs/20261027T101416Z/`.

**What worked [V].**
- The policy identified `11 f0 00 00 00 00`, the power handshake was ok on the first try,
  and the host role came up within about 0.6 s of the ID.
- The supplier offered an `ACA` 2100 mA source.
- After the unplug the gadget came back on the Mac.

**USB_CTL bit 1 did not stick [V].** The switch wrote bit 1 (`set by switch 1`), but
`USB_CTL+0` read `0x1` at the end of the host entry, and again after `reenter`. EHCI
`portsc1 = 00501000`, so there was no connect. The `0x3` hypothesis (§4.1) is
therefore **untested**. When or why the bit is lost is unknown **[I]**: write-ignored,
self-clearing, or cleared by a later step. usb-host5b (`bd387f2007`) does three things:
- It writes bit 1 before `phy_init()`, which is iOS's `setPowerLevel` order, and keeps it
  in the host-select write.
- It re-asserts the bit after EHCI and OHCI are up.
- It records `USB_CTL+0` after every step: entry, gadget-off, bit1, phy-on, host,
  phy-host, ehci, ohci, reassert. One line per entry goes to the log
  (`USB_CTL+0 trace: …`) and to debugfs `regs`.

The debug knob `usb_ctl_force=3` writes `0x3` once host mode is up and logs the
readback immediately, after 1 ms and after 100 ms. debugfs `apple-usb-role/usb_ctl`
does the same on demand. So the next run tests the hypothesis directly, whatever the
policy path does.

**Charging stopped near full, not because of host mode [V].**
- `j71t_battery_safe()` rejected battery voltages above **4.2 V** (`-ERANGE`). This
  cell is full above that: iOS on this unit reports **4256–4274 mV at 100 %**, and Linux
  read 4249 mV at 98 % while charging.
- The result was a 30 s cycle, already visible with the Mac cable at 98–99 %. The guard
  fired (`stepped down: -34`), the 100 mA handoff (`0xa03 = 2`) was restored, the voltage
  sagged below 4.2 V, and charging re-enabled 30 s later.
- In host mode the charger did ramp 500 → 1000 → 2100 mA (input code `0xa0`, reads
  2075000 µA). The battery charged at +656 mA, and 15 s later the same guard cut it at
  4.249 V.
- The first host entry also hit `charging policy ownership fault` (`-117`) about 1 s after
  the adapter attached, with a 60 s lockout. The same thing happened 0.15 s after a fresh
  source in the usb-host4 run. So the PMIC changes its own input settings right after an
  attach **[I]**.

usb-host5b fixes:
- The battery window ceiling is raised to **4.4 V** (`e3fda1a31d`). The PMIC still owns
  charge voltage and termination.
- The charger re-asserts its budget once per 10 s after a register change and logs which
  registers changed. A second change inside the window is still a fault (`2a6b8bfa87`).
- ACA is listed in the charger's USB types, which ends the
  `unavailable enum value 4` log flood.
- `lightning_state` shows the battery voltage (`2801e838fc`).

Re-scoring the 11:17 run with the corrected verdict gives `charging_host` and `input_2100`
PASS, and `input_held` FAIL (the input fell back to 100 mA in snapshot 021).
