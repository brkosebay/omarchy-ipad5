# Lightning-port USB host mode on J71t (Apple A9)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**2026-10-25 update:** use [usb-host.md](usb-host.md) for the built morning
trial, prior host-controller results, and corrections to this draft's IRQ/PHY
address discussion, four-byte capability reply, and accessory-rail versus
5 V VBUS interpretation.

2026-10-24. What it takes to run the Lightning port as a USB **host** (keyboards,
mice, flash drives through Apple's Lightning-to-USB Camera Adapter) and still keep
the device/ECM gadget when no adapter is attached.

**Scope.** Offline only: no device access, no register writes, no commits. Sources:
this unit's ADT (`build/firmware/private/IODeviceTree.txt`, `docs/adt/j71t.txt`),
the live iOS 16.7.10 (20H350) `IOService.txt` dump taken in device mode, the shared
Ghidra analysis of `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`
(image base `fffffff005298000`), and the kernel tree in the `linux-a9-src` volume
(`hoolock` + `a9/periph`).

**Tags.** **[V]** verified — the ADT/live dump/decompiled code/the Linux tree shown
below says exactly this. **[I]** inferred — consistent with the evidence, not proven
on hardware. Anything not marked is [I].

This file replaces the 2026-10-23 draft (`ce7ee73`). The corrections are listed in
§7; the load-bearing one is that an accessory **asks** for the switch with a USB
vendor request, and that the DWC2 core is not the host controller.

---

## 1. Answer in one page

1. **Host is EHCI + OHCI, not DWC2.** The A9 USB2 block is three controllers behind
   one PHY: a Synopsys DWC2 wired as a **device** (`usb-device`, `0x20c100000`), and
   a DesignWare EHCI (`0x20c200000`) + its OHCI companion (`0x20c300000`). iOS
   publishes exactly one set at a time, chosen by the DT `publish-criteria` property
   (device = 3, both host controllers = 4). Apple's DWC2 kext has no host code at
   all. **[V]**
2. **The adapter requests the swap.** It first enumerates the iPad as a USB *device*,
   selects a configuration that exposes the `IapOverUsbHid` interface, and sends
   vendor request **0x51** on it. `IOAccessoryPortUSB` handles the request, waits
   50 ms, then calls `IOAccessoryManager::configureUSBMode(3)`, which moves the
   USB2/AID-Bus transport states to "host". **[V]**
3. **The arbitrator swaps controller sets, the PHY does the electrical switch.**
   `updatePublishedNubs()` terminates the `usb-device` nub (killing DWC2, PTP, mux,
   NCM/ECM) and starts `usb-ehci0` + `usb-ohci0`. `AppleS5L8960XUSBPhy::enableHostMode`
   then sets `USB_CTL+0` bit 0, writes the host trim words into the PHY, and pulses
   the PHY reset. **[V]** for the code paths; the *electrical* effect of `USB_CTL`
   bit 0 is **[I]** until measured.
4. **VBUS comes from the PMU, with a Tristar gate.** The `dock` node wires
   `function-acc_pwr` / `function-acc_sw_en` to D2365 local register 5, and
   `AppleTriStarBuiltIn` can additionally set Tristar register **0x05 bit 0**
   ("VBUS sink") over I²C, refusing while the ID bus is in reset. **[V]** for the
   properties and the register write, **[I]** for the ordering/meaning.
5. **Linux already has most of the pieces, in the wrong shape.** The DTS has the
   DWC2 node with `dr_mode = "peripheral"` and a complete `phy-apple-ausb` driver
   whose host-mode `set_mode` is compiled out (`#if 0`); the EHCI/OHCI nodes exist
   but are inside `#if 0`; `dwc2` has full `usb-role-switch` support
   (`CONFIG_USB_DWC2_DUAL_ROLE=y`); nothing owns `USB_CTL` (`0x20e0d8000`). **[V]**
6. **Do one read first.** `devmem 0x20c100048 32` → `GHWCFG2.OP_MODE`:
   `3`/`4` means the DWC2 is device-only and the EHCI/OHCI path is mandatory;
   `0`/`1`/`2`/`5`/`6` means a DWC2 host experiment is also possible. **[I]** for
   the expected value (`4`), **[V]** that this register decides it.

---

## 2. Hardware map

All USB addresses are offsets inside `usb-complex` (`ranges` base `0x20c000000`,
size `0x600000`), except `USB_CTL`, which is a separate `otgphyctrl` register bank.

| Address | Size | DT node | Role |
|---|---|---|---|
| `0x20c000030` | 0x20 | `otgphyctrl` reg[0] | AUSB PHY: `CTL +0x00`, `SIG +0x04`, `CFG0 +0x08`, `CFG1 +0x0c`, trim `+0x1c` |
| `0x20e0d8000` | 0x1000 | `otgphyctrl` reg[1] | `USB_CTL`: `+0x00` bit 0 = host path, `+0x04` bit 1 = device enable |
| `0x20c100000` | 0x10000 | `usb-device` | DWC2, IRQ 214 (Apple) / 162 (Linux AIC) |
| `0x20c200000` | 0x10000 | `usb-ehci0` | EHCI host, IRQ 215 / 163, `port-current-limit 50` |
| `0x20c300000` | 0x10000 | `usb-ohci0` | OHCI companion, IRQ 216 / 164, `port-current-limit 50` |
| `0x20c900000` | 0xa0 | `usb-complex` | arbitrator; `usb_widget` AXI table |

Accessory side: Tristar CBTL1610 at **i2c0 `0x1a`** (`0x20a110000`), AP IRQ 72 with
the D2365 PMU as interrupt parent, `dock = <132>`. **[V]**

`usb-complex` children and their `publish-criteria` **[V]**:

```
usb-device   criteria 3   num-of-eps 14   fifo-depth 0x820
usb-ehci0    criteria 4   port 1   port-type 2   port-current-limit 50
usb-ohci0    criteria 4   port 1   port-type 2   port-current-limit 50
```

**USB 2.0 only.** This ADT has no `usb-ehci1`/SuperSpeed node (unlike J127/J128), so
the Lightning pins are one USB2 port. The USB3 Camera Adapter works, but its hub and
any drive run at 480 Mb/s. **[V]** for the ADT; the "USB3" product name is then just
compatibility.

`cfg0` and `cfg1` trim words for this unit **[V]**:

| Property | Value | Used in |
|---|---|---|
| `cfg0-device` | `0x37377bc3` | device mode |
| `cfg1-device` | `0x00020c04` | device mode |
| `cfg0-host` | `0x37373b03` | host mode (differs from device in bits 6, 7, 14) |
| `cfg1-host` | `0x00020c04` | host mode (identical to device) |
| `errata` | 0 | DM-pulldown workaround is dead on this board |

The words are per-SKU: j71s/j72s (Samsung S8000) have `errata = 2`,
`cfg0-device = 0x37477bb3`, `cfg1-device = cfg1-host = 0x00020e0c`, and their
`cfg0-host` was mis-decoded as the string `"##G7"` by the ADT dumper — re-extract
before ever running this on those boards. **[V]**

---

## 3. What iOS does when the camera adapter is plugged in

### 3.1 The request

`IOAccessoryPortUSB` matches `IOPropertyMatch = { USBDeviceFunction = IapOverUsbHid }`
on an `IOUSBDeviceInterface` — i.e. it is a function of the **iPad's own** device-mode
configuration, not of the host stack. **[V]**

Its request handler (`fffffff006c3d61c`, decompiled) implements three vendor requests:

| `bmRequestType` | `bRequest` | Behaviour |
|---|---|---|
| `0x40` | `0x51` | "Starting role switch" — arm the pending mode, schedule the swap |
| `0xc0` | `0x53` | return the 16-bit capability mask, byteswapped |
| `0x40` | other | `kIOReturnUnsupported` |

For `0x51` the handler validates `wValue`/`wIndex` against masks the driver
configured from the interface descriptors, sets the pending-mode flag, schedules a
thread call after `_roleSwitchDelayMS` (default **50 ms**, overridable by the DT
property `usb-role-switch-delay-ms` or the boot arg `acc_roleswap_delayms`), and
notifies other accessory clients with `messageClients(0xe3ff8022, request, 16)`.
**[V]**

The delayed worker (`fffffff006c3d5c4`, disassembled) does one thing:
`IOAccessoryManager::configureUSBMode(3)`, and on failure logs
`"USB role switch configureUSBMode() failed %#x"`. **[V]**

So the sequence is: adapter enumerates iPad → IAP interface → vendor 0x51 → 50 ms →
`configureUSBMode(3)`. No register is touched by the adapter itself. **[V]** for the
code path; that *the camera adapter* is the sender is **[I]** until captured on the
wire (a USB analyser on the Lightning pair, or the iOS-side mux log).

### 3.2 configureUSBMode(3)

`IOAccessoryManager_configureUSBModeGated` (`fffffff005df2228`, decompiled) stores
`_usbMode = 3`, then for transports 2 (`USB2`) and 3 (`AIDBus`) computes

```
mode 0,1,5 -> { a = 1, b = 1 }
mode 2,3   -> { a = 1, b = 2 }
mode 4, >=6-> { a = 0, b = 0 }
```

sets the `mode == 3` flag, sets the `mode == 1` flag on the transport, and calls
`updateState()` + `publishState()`. **[V]** The live dump shows
`IOAccessoryUSBModeType = 0` with `IOPortTransportStateUSB2.DataRole = 1` ("Device"),
so `b == 2` is the host state. **[I]**

`usb_connect_shim_handle_chrg` (`fffffff005e369c4`) has a second entry into the same
place: CHRG type 5 does `FUN_5df3d20(port, 1)` + `configureUSBMode(3)`. **[V]** That is
the path for a dumb accessory/charger that does not speak the vendor request.

### 3.3 The controller swap

`AppleEmbeddedUSBArbitrator::updatePublishedNubs` (`fffffff0060770f8`, decompiled)
walks the `usb-complex` children and does, per child:

```
sel = child.publish-criteria & mask
sel == 0 && nub  -> nub->terminate(1)              /* immediate */
sel != 0 && !nub -> create nub, attach, start
                    if criteria & 4: "AAPL,power-supply" = 0x32
```

With J71t's criteria (3 vs 4) this is a strict either/or, and `terminate(1)` on
`usb-device` takes down the whole device stack: DWC2, PTP, `AppleUSBMux`,
NCM/ECM, and the `IapOverUsbHid` interface that asked for the switch. **[V]**

When the mask has any of bits 1–2 set (`mask & 6`), the host nubs are published
*before* the cable type is announced; with a device mask the announcement comes
first. Functionally irrelevant for Linux. **[V]**

### 3.4 The electrical switch

`AppleS5L8960XUSBPhy::enableHostMode` (`fffffff006cbfd58`) — the only place the role
is turned into register writes. **[V]**, read instruction by instruction:

```
if (!_powerLevel) return;
clockGate(ref, 0x10)                       /* PMGR USB gate, held across the sequence */

if (host) setBits  (USB_CTL, 0x00, 0x00000001)
else      clearBits(USB_CTL, 0x00, 0xfffffffe)

write PHY+0x08 = cfg0_host (or cfg0_device)
write PHY+0x0c = cfg1_host (or cfg1_device)
write PHY+0x1c = 0
setBits(PHY+0x00, 1);  IODelay(20)
clearBits(PHY+0x00, 0xfffffffe);  IODelay(1000)
if (errata & 1) performJitterImprovement()  /* 0x2000,0x30dd,0x3511,0x3533 -> PHY+0x1c */

clockGate(ref, 0)
```

`enableDeviceMode` (`fffffff006cbfc40`) is only a set/clear of
bit 1 in `USB_CTL + 4` (`0x2` / `0xfffffffd` masks) inside the same clock gate.
**Nothing in the code clears
`USB_CTL+4` when going to host** — whether it must be cleared is an open question
(§6, test 5).

### 3.5 VBUS and the accessory rail

Two independent things:

- **The 5 V rail** is a PMU function, not a Tristar register. The J71t `dock` node
  (phandle 132) supplies `function-acc_pwr = 79:pmuL(0x5)`,
  `function-acc_sw_en = 79:pmuL(0x80000005)`, `function-acc_sleep_pwr = 79:pmuL(0x220)`
  to `AppleTriStarBuiltIn`, which looks up exactly those property names. **[V]**
  The `0x80000005` form most likely carries a write-enable in bit 31. **[I]**
- **A Tristar gate**: `setVBUSSink(bool)` (`fffffff005ff5214`, decompiled) writes
  **register 0x05, bit 0** via the IC write slot, caches it, and sleeps **18 ms**
  after enabling. It refuses with `0xe00002d5` and logs
  `"cannot set VBUS sink (%d) because ID_BUS is in reset"` while the ID bus is held
  in reset. `getVBUSSink` reads the same bit, unless the dock node has a
  `downstream-vbus` property (J71t does **not**). **[V]** for the code, **[I]** for
  "this is what turns the connector's VBUS on".
- J71t has no `supports-high-current` and no `function-usb_hs_*`, so only the
  100/500 mA class is wired; iOS advertises `AAPL,power-supply = 0x32` (100 mA) on the
  host nubs. **[V]** values, **[I]** meaning.

Practical ordering for Linux (unverified): `acc_sw_en` → `acc_pwr` → Tristar reg 0x05
bit 0 → PHY/USB_CTL; reverse on the way back.

### 3.6 Accessory identification is a separate problem

`IOAccessoryID` (91 live), `IOAccessoryDigitalID`, the accessory serials and
`IOAccessoryOrientation` come from the **AID bus** (the Lightning ID pin driven as a
slow I²C by `AppleTriStarIC`), not from any Tristar register. The registers we have
decoded (`0x0c` VBUS/orientation, `0x0d` events, `0x0f` revision `0x8a`) do not carry
it. **[V]** Reimplementing ID-bus identification is a much larger job than host mode;
don't put it on the critical path (§4). The protocol reference for that job now
exists — [lightning-idbus.md](lightning-idbus.md) (wire protocol, CRC8, GetID
exchange, ACCx/Dx routing semantics, power handshakes).

---

## 4. What Linux has today

Verified against the kernel tree in the `linux-a9-src` volume (hoolock head):

```dts
/* s5l8960x.dtsi, inherited by s8000/j71 */
ausbphy0: phy@20c000020 {                     /* <-- see the address warning below */
        compatible = "apple,s5l8960x-ausb-phy", "apple,ausb-phy";
        reg = <0x2 0x0c000020 0x0 0x20>;
        power-domains = <&ps_usbctrl>;
        #phy-cells = <1>;
};

usbdev: usbdev@20c100000 {
        compatible = "apple,s5l8960x-dwc2", "apple,dwc2";
        reg = <0x2 0x0c100000 0x0 0x10000>;
        interrupts = <AIC_IRQ 162 IRQ_TYPE_LEVEL_HIGH>;
        clocks = <&clkref>;  clock-names = "otg";
        phys = <&ausbphy0 PHY_TYPE_USB2>;  phy-names = "usb2-phy";
        power-domains = <&ps_usbotg>;  resets = <&ps_usbotg>;
        dr_mode = "peripheral";
};

#if 0
ehci0: usb-ehci@20c200000 { compatible = "apple,s5l8960x-ehci", "generic-ehci";
        reg = <0x2 0x0c200000 0x0 0x10000>; interrupts = <AIC_IRQ 163 ...>;
        power-domains = <&ps_usb2host0>; };
ohci0: usb-ohci@20c300000 { compatible = "apple,s5l8960x-ohci", "generic-ohci";
        reg = <0x2 0x0c300000 0x0 0x10000>; interrupts = <AIC_IRQ 164 ...>;
        power-domains = <&ps_usb2host0_ohci>; };
/* ehci1/ohci1 at 0xc400000/0xc500000 do not exist on J71t */
#endif
```

| Piece | State | Consequence |
|---|---|---|
| `phy-apple-ausb` (`drivers/phy/apple/ausb.c`) | present; `ausb_phy_usb2_set_mode()` exists but is **`#if 0`** | host trim words and `AUSB_SIG_HOST (7<<12)` are already written; only the hook is missing |
| `dwc2` | `CONFIG_USB_DWC2=y`, `DUAL_ROLE=y`, `HOST`/`PERIPHERAL` off, `dr_mode="peripheral"` | `drivers/usb/dwc2/drd.c` and `usb-role-switch` support are compiled in |
| `CONFIG_USB_ROLE_SWITCH` | `y` | `/sys/class/usb_role/*/role` exists as soon as a provider registers |
| EHCI/OHCI | nodes `#if 0`; `ehci-platform`/`ohci-platform` are built | `generic-ehci`/`generic-ohci` would bind once the nodes are enabled |
| PMGR domains | `ps_usb`, `ps_usbctrl`, `ps_usbotg`, `ps_usb2host0`, `ps_usb2host0_ohci` all defined | no new clock/power plumbing needed |
| `0x20e0d8000` (`USB_CTL`) | **no DT node, no driver** | must be added; it is outside `usb-complex` ranges |
| Tristar (i2c0 0x1a) | read-only supplier (`scripts/periph/kernel/apple_tristar_power.c`, patches 0040/0050) | the natural home for the role switch |

**Address warning [V]:** the ADT and the live iOS `IODeviceMemory` both put the PHY
block at **`0x20c000030`** (32 bytes), and m1n1 (`src/usb.c` picks the reg entry with
size `0x20`) programs `CFG0` at base+0x08 = `0x20c000038` on every boot. The shared
A7/A9 DTSI says `0x20c000020`, so `phy-apple-ausb` writes `0x20c000028`. Both cannot
be the same register. **[V]** Because m1n1 has already written the device words at
`0x38/0x3c` before Linux runs, device mode still works and the mistake stays invisible
— **[I]**, but it means a host-mode `cfg0` write through mainline's address would go
nowhere. Verify with `devmem 0x20c000028 32` / `devmem 0x20c000038 32` (§6, test 1) and
override `reg` in the A9 DTSI if needed. m1n1's own comment states the architecture
plainly: *"A7-A11 uses a custom internal otg phy with the peripheral part being dwc2,
role switch seems custom."* **[V]**

---

## 5. Minimal implementation

### 5.1 Decide the controller first (one read)

`GHWCFG2` is at DWC2 `+0x048` (`0x20c100048`); `OP_MODE` is bits 2:0:

| value | meaning | useful here |
|---|---|---|
| 0, 1, 2 | OTG (host + device) | yes — Path A is possible |
| 3, 4 | device-only | **no** — Path B mandatory |
| 5, 6 | host-only | no — the gadget would stop working |

Expected: `4` (`NO_SRP_CAPABLE_DEVICE`), because Apple ships separate EHCI/OHCI. If it
is 3/4, enabling `CONFIG_USB_DWC2_HOST` makes `dwc2_get_dr_mode()` fail probe with
`"Controller does not support host mode."` — a safe, self-diagnosing experiment that
settles it even if the `devmem` read is ambiguous.

### 5.2 Path A — DWC2 as host (only if 5.1 says host-capable)

Smallest diff, no new platform devices:

```dts
&usbdev {
        dr_mode = "otg";
        usb-role-switch;
        role-switch-default-mode = "peripheral";
};
```

plus a provider that, in this order, (1) powers the accessory rail, (2) puts the PHY
in host mode, (3) sets `USB_CTL+0` bit 0, (4) calls `usb_role_switch_set_role(HOST)`
last. `dwc2`'s `drd.c` then sets `GUSBCFG.FORCEHOSTMODE`, resets the core and
registers an HCD; `HPRT0` is at `+0x440`. Note `dwc2` never calls `phy_set_mode()`,
so the provider — not the PHY core — must do step 2.

### 5.3 Path B — EHCI + OHCI (expected, and the faithful one)

This mirrors iOS exactly and reuses two upstream host drivers:

1. **Enable the two host nodes** (drop the `#if 0`, add `status = "okay"`, but keep
   them out of the default boot state — see 5.5).
2. **A `usb-complex` role driver** (new, J71t-only, or a `usb-role-switch` provider in
   `apple_tristar_power.c`) that owns the transition:

   ```
   enter_host():
       unbind dwc2 (device_release_driver)           /* iOS: nub->terminate(1) first */
       regulator_enable(acc_pwr)                     /* D2365 local reg 5 */
       tristar_write(0x05, cached | 1); msleep(18)   /* VBUS sink */
       phy_set_mode(ausbphy0, PHY_MODE_USB_HOST)     /* cfg0/cfg1 host */
       usb_ctl_set_bit(0)                            /* 0x20e0d8000 */
       register ehci0 + ohci0 platform devices
       usb_role_switch_set_role(sw, USB_ROLE_HOST)

   enter_device():
       unregister ehci0 + ohci0
       usb_ctl_clear_bit(0); clear USB_CTL+4 bit 1 if test 5 says so
       phy_set_mode(ausbphy0, PHY_MODE_USB_DEVICE)
       tristar_write(0x05, cached & ~1)
       regulator_disable(acc_pwr)
       device_attach(usbdev)                         /* gadget comes back */
       usb_role_switch_set_role(sw, USB_ROLE_DEVICE)
   ```

   Registering the host controllers at runtime (instead of via DT `status`) is what
   keeps `usb-device` and `ehci0` from ever being probed together, which is the one
   invariant iOS enforces with `publish-criteria`.
3. **`USB_CTL` node**: `compatible = "apple,s8000-usb-ctl", "syscon"`,
   `reg = <0x2 0x0e0d8000 0 0x1000>`, consumed by the role driver through a regmap.
4. **PHY**: un-`#if 0` `ausb_phy_usb2_set_mode()` in `ausb.c`, and add
   `phys = <&ausbphy0 PHY_TYPE_USB2>; phy-names = "usb";` to the EHCI/OHCI nodes so
   the mode is set even if the role driver is not the one calling it.
5. **`ehci-platform`/`ohci-platform`** are the bind targets for `generic-ehci`/
   `generic-ohci`. Expect to need Apple port glue (per-port power/reset, the
   `AppleUSBHostT7000` equivalents) only if the ports come up dead; that glue has not
   been reverse engineered and is the main risk in this path. **[I]**

### 5.4 Manual switch first (no driver)

Both paths can be exercised by hand in one boot with `devmem` + a tiny module that
only creates the two platform devices. That is also the fastest way to answer the
`USB_CTL`/EHCI ownership question. Do **not** put this in a script that runs on boot.

### 5.5 Keeping the gadget/ECM path

- Default role is `device`; nothing changes until someone writes
  `echo host > /sys/class/usb_role/<name>/role`.
- The transition must unbind the UDC before `USB_CTL` changes, exactly like iOS
  `terminate(1)`. The Mac then sees a disconnect, which is correct and expected.
- On return, re-attach the DWC2 platform device and let configfs re-bind
  `g_ether`/`g_multi`; the ECM address on the Mac side may need a `dhclient`/`ip link`
  bounce. Keep a 30 s watchdog that forces `device` back if userspace dies.
- **Operational warning:** the ECM gadget is the only network path to the iPad. The
  first host-mode experiments must run from a serial/UART console (or with an
  auto-revert timer), or the session is lost until reboot.

### 5.6 DT sketch (Path B)

```dts
&{/soc} {
        usb_ctl: usb-ctl@20e0d8000 {
                compatible = "apple,s8000-usb-ctl", "syscon";
                reg = <0x2 0x0e0d8000 0 0x1000>;
        };
};

&ausbphy0 { reg = <0x2 0x0c000030 0x0 0x20>; };   /* only if §6 test 1 says so */

&i2c0 {
        periph_tristar: power-controller@1a {
                compatible = "apple,j71t-cbtl1610-power";
                reg = <0x1a>;
                usb-controller = <&usbdev>;
                usb-role-switch;                  /* provider side */
                usb-ctl = <&usb_ctl>;
                usb-port-supply = <&acc_pwr>;     /* D2365 reg 5 as a regulator */
        };
};

&ehci0 { status = "disabled"; };   /* enabled at runtime by the role driver */
&ohci0 { status = "disabled"; };
```

`acc_pwr` should be a `regulator-fixed` (or a small D2365 function regulator) so the
role driver only calls `regulator_enable()`; do not open-code PMU register 5 in a
second place.

---

## 6. Device test plan

Read-only until step 5. Record every value in a table in this file; the numbers matter
more than the narrative.

**1. Register baseline, no adapter, device mode.** Read every value as 32-bit
(`devmem ADDR 32`; a bare `devmem ADDR` reads 64 bits on arm64).

```
devmem 0x20c100048          # GHWCFG2 -> OP_MODE, decides §5.1
devmem 0x20c100440          # HPRT0
devmem 0x20c100000          # GOTGCTL
devmem 0x20c000020 ; devmem 0x20c000028 ; devmem 0x20c000030 ; devmem 0x20c000038
devmem 0x20c000034 ; devmem 0x20c00003c ; devmem 0x20c00004c   # the 0x10 offset question
devmem 0x20e0d8000 ; devmem 0x20e0d8004                        # USB_CTL
i2cget -y 0 0x1a 0x0f ; i2cget -y 0 0x1a 0x05 ; i2cget -y 0 0x1a 0x0c ; i2cget -y 0 0x1a 0x0d
```
Expect: `USB_CTL+0` bit 0 clear, `USB_CTL+4` bit 1 set, Tristar `0x0f = 0x8a`,
`0x05` bit 0 clear. If `0x20c000038` shows `0x37377bc3` and `0x20c000028` does not,
mainline's PHY base is off by 0x10 (§4).

**2. Same reads with the camera adapter plugged in, nothing attached, iOS still
running.** Diff against step 1. Confirms whether `0x05` bit 0 changes, and whether
`USB_CTL`/PHY move at all before a device is attached.

**3. Same reads with a keyboard in the adapter** — this is the state where iOS has
already switched. Expect `IOAccessoryUSBModeType = 3`, `USB_CTL+0` bit 0 set,
`cfg0` = `0x37373b03`, `0x05` bit 0 set, `AppleUSBHostController`/`IOUSBHostDevice`
non-zero in `ioreg`. Capture `ioreg -l -w0 -p IOService -n AppleTriStarBuiltIn` and
the `IOPortTransportStateUSB2` object.

**4. Linux, device mode, adapter attached.** Same `devmem`/`i2cget` set as step 1.
The gadget must still enumerate on the Mac (that is the regression check). If the
adapter alone disturbs the link, stop here and report.

**5. Manual host switch.** From a serial console, with the UDC unbound:

```
echo "" > /sys/class/udc/*/UDC
# PHY first (offset base 0x20c000030): cfg0 = 0x37373b03, cfg1 = 0x00020c04,
# trim +0x1c = 0, then pulse +0x00 bit 0 (high, 20 us, low, 1 ms).
v=$(devmem 0x20e0d8000); devmem 0x20e0d8000 32 $((v | 1))   # host select
```
Watch `HPRT0` (`0x20c100440`, bit 0 connect, bit 12 power) **and** EHCI `PORTSC`
(`0x20c200044`). Whichever reports the connect owns the pins. Also read back
`USB_CTL` and the PHY to prove nothing re-tuned them. If neither sees the keyboard,
repeat with `USB_CTL+4` bit 1 cleared.

**6. VBUS measurement.** Multimeter on the adapter's VBUS: (a) rail on, `USB_CTL`
bit 0 clear; (b) rail on, bit 0 set; (c) `0x05` bit 0 set as well. If VBUS only
appears with (b) or (c), that is the real switch and §3.5 must be rewritten.

**7. Driver host mode + enumeration.** `echo host > /sys/class/usb_role/*/role`,
then `dmesg | grep -E 'ehci|ohci|usb [0-9]'`, `lsusb -t`, plug hub / keyboard / stick.
Success = the device tree on the bus and working input or storage.

**8. Return + stress.** `echo device > .../role`: the gadget must reappear on the Mac
within one enumeration and `USB_CTL` must match step 1. Then 20 host↔device cycles and
20 plug/unplug cycles; check `regulator_summary` for a leaked `acc_pwr` and the UDC
for a failed rebind.

**9. Power interaction.** In host mode the iPad is sourcing, so the charger policy
must not try to charge from that port: `cat /sys/class/power_supply/usb/current_max`,
`.../charger/input_current_limit`, and confirm
`docs/periph/ds-charging-policy.md`'s source budget is not re-armed while the role is
host.

---

## 7. Corrections to the 2026-10-23 draft

| Old claim | Now |
|---|---|
| "the switch is two register writes" | three layers: vendor request/port state, arbitrator nub swap, then PHY+`USB_CTL` |
| "accessory ID → cable type decides host" | the adapter **asks** with vendor request `0x51`; the CHRG-type-5 path is the fallback for dumb accessories |
| "PHY lives at `0x20c000030`" | true for iOS (`IODeviceMemory`) and m1n1; mainline DTS says `0x20c000020` — unreconciled, test 1 |
| "VBUS is only the PMU rail" | PMU rail **and** Tristar reg `0x05` bit 0 (`setVBUSSink`, 18 ms settle) |
| "EHCI/OHCI nodes do not exist in Linux" | they exist in the DTSI, `#if 0`, with PMGR domains wired |
| no Linux-side detail | `phy-apple-ausb`, `dwc2` drd, `usb_role` class, `USB_CTL` gap all documented above |

## 8. Open questions

1. `GHWCFG2.OP_MODE` — the one read that decides the implementation (§5.1).
2. Does `USB_CTL+0` bit 0 hand the pins to EHCI/OHCI, or to a DWC2 host? (§6 test 5)
3. Must `USB_CTL+4` bit 1 be cleared in host mode? iOS never touches it.
4. Is Tristar `0x05` bit 0 the connector VBUS switch, or only a signal to the
   accessory? (test 6)
5. Real `0x20c000028` vs `0x20c000038` PHY base on A9.
6. `AUSB_SIG` bits 12–14 (`AUSB_SIG_HOST` in mainline) — Apple's host path does not
   write them; do they matter on this PHY?
7. Does `ehci-platform` drive the Apple EHCI port correctly without Apple's
   `AppleUSBHostT7000` glue? (risk in Path B)

## 9. Evidence pointers

- Decompiled/disassembled for this note: `fffffff006c3d61c` (vendor requests),
  `fffffff006c3d5c4` (role-switch worker), `fffffff005ff5214` (`setVBUSSink`),
  `fffffff005df2228` (`configureUSBModeGated`), `fffffff005df3d20`/`5df3d8c`
  (pending-mode flag), `fffffff0060770f8` (`updatePublishedNubs`).
- `scripts/ghidra.py decompile_function_by_address address=fffffff006c3d61c`
  (the shared server was up; no restart, no save). `enableHostMode` was re-read from
  `build/periph/usbhost-kexts/AppleS5L8960XUSB.macho` with
  `objdump -d --start-address=0xfffffff006cbfd58 --stop-address=0xfffffff006cc0080`.
- Extracted kexts and strings: `build/periph/usbhost-kexts/` (kernelcache SHA256 in
  `manifest.json`).
- Live state: `build/firmware/private/IOService.txt` (device mode, Mac attached),
  `build/firmware/private/IODeviceTree.txt`.
- Linux side: `drivers/phy/apple/ausb.c`, `drivers/usb/dwc2/{platform,drd}.c`,
  `arch/arm64/boot/dts/apple/s5l8960x.dtsi`, `s5l8960x-pmgr.dtsi`, and m1n1
  `src/usb.c` + `src/usb_complex.h` in the `linux-a9-src` volume.- Related: [tristar-recon.md](tristar-recon.md) (I²C transport, `0x0c/0x0d/0x0f`),
  [ds-charging-policy.md](ds-charging-policy.md) (receive-side budget),
  [round5-host-recon.md](round5-host-recon.md) (why "USB host" is not a 2.4 A grant),
  [round4-usb.md](round4-usb.md) (current gadget implementation).
