# iAP-over-HID role switch on J71t

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Status: iOS 16.7.10 (`kernelcache.release.iPad6,11_12`) reverse engineering,
plus the saved live IORegistry and Mac descriptor dump. This document uses:

- **[V]** verified in the kernelcache or a saved descriptor/registry capture;
- **[I]** inferred, including values not present while configuration 2 was inactive;
- **[E]** an external protocol/specification statement.

The implementation target is deliberately small: expose enough of Apple's
device-mode iAP HID function for the Camera Adapter to issue the vendor role
request. It does not implement iAP authentication or a complete iAP transport.

## 1. Device and configuration

The live `AppleNubSynopsysOTGDevice` description and the Mac capture agree on
the device descriptor below. **[V]**

| Field | Value |
|---|---:|
| `bcdUSB` | `0x0200` |
| `bDeviceClass/SubClass/Protocol` | `0/0/0` |
| `bMaxPacketSize0` | 64 |
| `idVendor` | `0x05ac` |
| `idProduct` | `0x12ab` |
| `bcdDevice` | `0x0611` |
| manufacturer | `Apple Inc.` |
| product | `iPad` |
| serial | the device's 40-hex-character unique ID (not reproduced here) |
| configurations visible to the Mac | 5 |

The internal device description contains eight configuration records, of which
some are marked extended or locked. Its second record is named **`iPod USB
Interface`**, has `AccessoryResistorSwap=Yes`, and lists the functions in this
order: `USBAudioControl`, `USBAudioStreaming`, `IapOverUsbHid`. No other record
contains `IapOverUsbHid`. **[V]** The old MFi hardware specification likewise
defines the iUI as configuration value 2; treating this internal second record
as on-wire `bConfigurationValue=2` is therefore **[I] [E]**.

The live registry creates `IapOverUsbHid@3`, and the driver asks the device
configurator for interface 3. Thus the HID interface is interface number 3,
alternate setting 0, and is the third *function* after the audio functions.
The audio streaming function consumes more than one interface number. **[V]**

`IOAccessoryPortUSB::start` sets the interface class tuple to `03/00/00`: HID,
no boot subclass, no protocol. It creates one interrupt-IN pipe with a 64-byte
maximum packet and interval 1. **[V]** The endpoint number is allocated by the
device configurator rather than embedded in this kext; it is expected to be
`0x81` in this configuration, but that address is **[I]**. FunctionFS is allowed
to remap the implementation's `0x81` request to a free physical endpoint.

The driver never calls the interface-string setter, so `iInterface=0`: there is
**no interface string**. `iPod USB Interface` is the configuration string, not
an interface string. **[V]**

The HID descriptor registered for full speed is: **[V]**

```text
09 21 11 01 00 01 22 60 00
```

That is HID 1.11, country 0, one report descriptor of length `0x0060`. The
corresponding 96-byte report descriptor at `fffffff007b3c6d1` is: **[V]**

```text
06 00 ff 09 01 a1 01 75 08 26 80 00 15 00
09 01 85 01 95 0c 82 02 01
09 01 85 02 95 0e 82 02 01
09 01 85 03 95 14 82 02 01
09 01 85 04 95 3f 82 02 01
09 01 85 05 95 08 92 02 01
09 01 85 06 95 0a 92 02 01
09 01 85 07 95 0e 92 02 01
09 01 85 08 95 14 92 02 01
09 01 85 09 95 3f 92 02 01 c0
```

The other-speed HID descriptor is: **[V]**

```text
09 21 11 01 00 01 22 d0 00
```

Its 208-byte report descriptor at `fffffff007b3c73a` is: **[V]**

```text
06 00 ff 09 01 a1 01 75 08 26 80 00 15 00
09 01 85 01 95 05 82 02 01
09 01 85 02 95 09 82 02 01
09 01 85 03 95 0d 82 02 01
09 01 85 04 95 11 82 02 01
09 01 85 05 95 19 82 02 01
09 01 85 06 95 31 82 02 01
09 01 85 07 95 5f 82 02 01
09 01 85 08 95 c1 82 02 01
09 01 85 09 96 01 01 82 02 01
09 01 85 0a 96 81 01 82 02 01
09 01 85 0b 96 01 02 82 02 01
09 01 85 0c 96 ff 02 82 02 01
09 01 85 0d 95 05 92 02 01
09 01 85 0e 95 09 92 02 01
09 01 85 0f 95 0d 92 02 01
09 01 85 10 95 11 92 02 01
09 01 85 11 95 19 92 02 01
09 01 85 12 95 31 92 02 01
09 01 85 13 95 5f 92 02 01
09 01 85 14 95 c1 92 02 01
09 01 85 15 95 ff 92 02 01 c0
```

The kext registers the 96-byte form for speed mask `0x01` and the 208-byte form
for `0x0e`; identifying those masks as full speed versus all other supported
speeds is **[I]**, corroborated by the adapter's observed full-speed connection.
The MFi specification independently describes iUI as a HID interface using
endpoint zero for OUT reports and one interrupt endpoint for IN reports. **[E]**

## 2. Control-request semantics

The handler is `IOAccessoryPortUSB_handleVendorRoleRequest` at
`fffffff006c3d61c`. The exact accepted role request is: **[V]**

```text
bmRequestType = 0x40   vendor, device recipient, host-to-device
bRequest      = 0x51
wLength       = 0
(wValue & ~valueMask) == 0
(wIndex & ~indexMask) == 0
```

This is a subset test, not equality with an expected value. The masks are set
through `IOAccessoryPortUSBRoleSwitchMaskValue` and
`IOAccessoryPortUSBRoleSwitchMaskIndex` properties handled by
`IOAccessoryPortUSB::setProperties`; they are **not derived from the interface
descriptor**. **[V]** No such properties appear in the saved live registry
while the iUI configuration is inactive, so both zero-initialized masks—and
therefore `wValue=wIndex=0`—are the best current J71t value. **[I]**

On acceptance the handler returns success with no data buffer, so the transfer
ends with a zero-length status stage. It marks the role change pending, emits
`messageClients(0xe3ff8022, request, 16)`, and schedules its worker after
`_roleSwitchDelayMS`, 50 ms by default. The property
`usb-role-switch-delay-ms` or boot argument `acc_roleswap_delayms` can override
that delay. **[V]**

The capability request is: **[V]**

```text
bmRequestType = 0xc0   vendor, device recipient, device-to-host
bRequest      = 0x53
wValue        = 0
wIndex        = 0
wLength       = 4
reply         = le16(valueMask), le16(indexMask)
```

Earlier notes describing a two-byte mask are wrong. The code stores the two
16-bit masks as `{indexMask, valueMask}`, rotates the containing 32-bit word by
16, and returns all four bytes. With the inferred default masks the reply is
`00 00 00 00`. **[V]** for layout and computation; **[I]** for the live values.

The same handler explicitly stalls HID `SET_IDLE` (`class OUT`, request
`0x0a`). It accepts HID `SET_REPORT` (`class OUT`, request `0x09`), buffers the
OUT payload, and passes bytes after the first two to its iAP parser. Unknown
requests are unhandled/stalled. **[V]** Nothing in the `0x51` branch checks an
authentication state, a prior report, or endpoint traffic; the control handler
is installed as soon as the interface starts. Thus no earlier iAP transaction
is required by this code path. **[V]** Whether this Camera Adapter sends a
`SET_REPORT` before `0x51` remains unmeasured **[I]**; the Linux responder logs
and consumes it so the device test will answer that question.

## 3. What happens after the status stage

The delayed worker at `fffffff006c3d5c4` calls only
`IOAccessoryManager::configureUSBMode(3)`. **[V]** The manager moves the USB2
and AID-bus transport states to host, after which the USB arbitrator removes
the DWC2 device nub and publishes the EHCI/OHCI nubs; the PHY host callback
selects host trims and `USB_CTL` state. **[V]**

Neither the request handler, delayed worker, nor the direct
`configureUSBMode(3)` gated path calls Tristar `setVBUSSink`, `acc_pwr`, or
`acc_sw_en`. **[V]** Those controls belong to the separate accessory
identification/power state machines. It is possible that transport-state
observers apply power policy elsewhere, but that has not been established for
this transition. **[I]** Consequently `listen none ...` is the faithful first
test; `pmu`, `tristar`, and `both` remain explicit experimental comparisons,
not claims about iOS ordering.

### 3.1 Before enumeration: GetID to the first USB reset

The 2026-10-26 device run closes an important boundary. The adapter repeatedly
reset DWC2 at full speed, but DWC2 never logged a new address and FunctionFS saw
only `BIND`, not `ENABLE` or `SETUP`. The same happened after a valid GetID
(`11 f0 00 00 00 00`) and with the temporary iOS device identity
`05ac:12ab`, `bcdDevice=0611`, `Apple Inc.` / `iPad`. **[V]** Therefore the
failure precedes configuration selection and the iAP descriptors: the adapter
either did not issue, or the gadget did not complete, the address-zero device
descriptor transaction. Standard `GET_DESCRIPTOR` and `SET_ADDRESS` are
handled by composite/DWC2 before FunctionFS is enabled. **[V]**

The iOS-side sequence around that boundary is:

1. CBTL1610 reports accessory-detect in event register `0x0d` bit 0;
   `AppleCBTL1610_handleInterruptEvent_reg0x0d` calls
   `AppleTriStarIC_handleAccessoryDetectEvent`. **[V]**
2. The ID-bus engine sends GetID command `0x74` with HOSTID, validates the
   `0x75` response and CRC, and forwards the six bytes to the dock/manager
   handlers. The observed adapter reply is `11 f0 00 00 00 00`. **[V]**
3. `IOAccessoryDock9Pin_decodeDigitalIDTransports` at `fffffff005e4d9ac`
   maps that ID's `USB=2` and `UART=1` fields to the `usb0` and `urt0`
   transports; `handleDigitalID` publishes `IOAccessoryDigitalID` and schedules
   the allowed-feature/transport reevaluation. **[V]** This decode path does
   not call the CBTL1610 `dxctrl` (`0x01`) or ACC mux (`0x02`) writers. **[V]**
   The general transport router can write `dxctrl` when it changes endpoint
   pairs, but that is a separate `IOPort` route operation, not part of GetID
   decoding. `AppleCBTL1610_configureTransportRoute` at
   `fffffff005fedfd4` delegates to the common encoder at
   `fffffff005ff4a08`, which supports `usb0`, `usb1`, `usbc`, UART, MikeyBus
   and JTAG endpoints and updates register `0x01`. **[V]** Both the Mac cable
   and Camera Adapter IDs request the same USB2 transport (`USB=2`); Linux's
   Mac tether enumerates at high speed with `0x01=0`, and the adapter completes
   a full-speed reset in that same state. A new permanent mux value is
   therefore not supported as a prerequisite. **[V]** The same allowed-route
   routine treats `PWR=f` separately: it selects the logical route pair
   `{idbs, apwr}`, where `PWR=0` selects `{idbs, 0}`. **[V]** This is an
   IOAccessory transport request, not a direct write to Tristar register
   `0x05`, PMU LDO5, or `acc_sw_en` in this function. Mapping `apwr` to the
   eventual physical rail policy is **[I]**. Linux nevertheless observed each
   post-handshake adapter reset with `accessory power: none`, PMU LDO5 off and
   Tristar `0x05=0`, so that rail is not required for the adapter to start the
   pre-`0x51` device enumeration. **[V]** It may still matter after a real role
   switch when the iPad must power the downstream flash drive. **[I]**
4. The separate dock power-policy state machine runs the now device-verified
   `0x72 -> 0x73 -> 0x70` exchange. Its response dispatcher at
   `fffffff005e4f168` accepts `0x73` and saves all four payload bytes. The
   continuation at `fffffff005e4f5d8` constructs `0x70` from only the first
   two saved bytes (with bit 3 of the first byte selected by policy). Thus the
   observed `73 00 00 c0 00` correctly produces `70 00 00`; `c0 00` is not
   interpreted there as a current, voltage, or accessory-power request.
   **[V]** On the successful `0x71`,
   `IOAccessoryDock9Pin_setPowerHandshakeState` at `fffffff005e4d644`
   clears `InflowDisable` and publishes `IOAccessoryPowerHandshake=true`.
   Neither the response path nor the completion function calls
   `configureAccessoryPower`, `acc_pwr`, `acc_sw_en`, `setVBUSSink`, or
   `configureUSBMode`. **[V]** The handshake therefore authorizes input-power
   policy; it is not itself a command to source unrequested accessory power or
   change the USB data role. **[I]** for the policy description.
5. There is an independent shortcut that can enter host mode without `0x51`:
   `usb_connect_shim_handle_chrg` at `fffffff005e369c4` calls
   `IOAccessoryManager_configureUSBMode(3)` when a charger callback reports
   type 5. **[V]** It is not a mapping of the Camera Adapter's PWR nibble.
   For Digital ID `11 f0 00 00 00 00`,
   `accessory_determine_usb_charger` at `fffffff005de8328` fails the special
   Digital-ID charger predicate (byte 0 bit 2 and byte 2 bit 3 are both clear)
   and falls back to D+/D- voltage classification. That classifier produces
   type/brick-ID 5 only when both samples are 2200--2879 mV. **[V]** The
   adapter's repeated USB reset demonstrates host pull-down signalling instead
   of that static charger bias, so it is not expected to take the type-5
   shortcut in this run. **[I]** Direct references to
   `IOAccessoryManager_configureUSBMode` are the CHRG shim, the ordinary
   manager work-loop entry, and the USB role-request thunk; neither the
   Digital-ID handler nor power-handshake handler calls it. **[V]**
6. Device-mode USB uses the ordinary Apple USB-device stack. On J71t,
   `AppleS5L8960XUSBPhy::enableHostMode(false)` selects the ADT's global device
   trims `cfg0=0x37377bc3`, `cfg1=0x00020c04`, clears trim and pulses reset.
   J71t's PHY `errata` value is zero. There is no accessory-ID or full-speed
   branch in this callback. **[V]** Linux already programs the same device
   values when its AUSB PHY powers on or changes to device mode. **[V]**

`configureUSBMode(1)` or `(2)` is not an omitted pre-enumeration step. The
saved live J71t state with the Mac attached has `IOAccessoryUSBModeType=0`,
`USB2 Active=Yes`, `DataRole=1` / `Device`; it enumerated normally. **[V]**
`IOAccessoryManager_configureUSBModeGated` maps modes 0, 1 and 5 to an active
USB2 device data role, modes 2 and 3 to an active host data role, and mode 4 to
inactive/no data role. Mode 1 additionally marks a legacy 30-pin accessory;
mode 3 is the role-switch target and marks the alternate host configuration.
**[V]** Thus the Camera Adapter starts against the already-active mode-0
device stack. Its Digital ID and power-handshake paths do not request mode 3;
the expected transition is request `0x51`, with the independent CHRG-type-5
shortcut described above as the other possible caller. **[V]**

### 3.2 usb-host4 and usb-host4b timing results

The successful power handshake supplied the first timing discriminator. In
three direct host trials the `0x71` completed at 147.258434, 174.214799 and
201.164052 seconds. DWC2 logged `new device is full-speed` 40.5, 42.2 and
42.8 ms later. In every case Linux had already begun unbinding the gadget to
force host mode. **[V]** The first listener likewise disconnected its UDC
across that 40--43 ms window. Those tests interrupted or hid the adapter's
device-mode retry; they do not show that iOS switches directly after the power
handshake. **[V]**

The cold-boot usb-host4b run exposed a second race. Swapping the Mac cable for
the adapter did not change the Tristar power-supply `online` value, and DWC2
retained the Mac's configured session. Manual GetID and the successful power
handshake consequently left the stale USB session untouched. Only a manual
soft-connect cycle produced the adapter's full-speed reset. **[V]** The Mac and
adapter can both report Tristar `0x0c=0xa3`; VBUS and orientation alone cannot
identify that replacement. A live GetID changed from Mac
`10 0c 00 00 00 00` to adapter `11 f0 00 00 00 00`, so GetID is the available
event source for this otherwise invisible cable change. **[V]**

The detailed DWC2 trace after that reconnect is: EP0's old request completes
with `-ECONNRESET`; the driver initializes the FIFOs; it queues an eight-byte
EP0 OUT setup request with `DOEPCTL0=0x80008000`; the adapter asserts USB reset;
`EnumDone` reports full speed; and two milliseconds later `EarlySusp` occurs.
There is no OUT-endpoint/setup interrupt, descriptor request, or SET_ADDRESS.
**[V]** `DSTS=0x00400002` says full speed (`ENUMSPD=1`), but it does **not**
prove SOF progress: Linux defines the SOF-frame field as bits 21:8, which are
zero in that value. The early-suspend interrupt is instead consistent with the
adapter ending bus traffic immediately after reset. **[V]** for the register
decode; **[I]** for that final interpretation.

The debug message `FIFOs reset, timeout at 99` is not a timeout. In
`dwc2_hsotg_init_fifo`, the counter starts at 100, is decremented after each
one-microsecond wait, and the error path is reached only at zero. A value of 99
means TX/RX FIFO flush completed after one wait. **[V]** A real failure would
also print `timeout flushing fifos (GRSTCTL=...)`; none occurs in the adapter
reset sequence. The separate `dwc2_hsotg_txfifo_flush: timeout flushing fifo`
warnings at 115.703 s occur while the driver tears down the stale, configured
Mac session. **[V]** Both successful high-speed Mac enumerations predate the
dynamic-debug capture, so absence of the harmless `timeout at 99` debug line
there is not a comparison; importantly, neither Mac enumeration logged the
real warning/error path. **[V]**

A separate control forced the Mac gadget to full speed: DWC2 logged full speed,
assigned address 1, and ECM/SSH worked at 12 Mbit/s. **[V]** Generic A9
full-speed PHY trims, termination, routing and EP0 therefore work. The adapter
failure is presently after reset/enum-done and before the first setup packet,
not in FunctionFS or the iAP descriptor facade. **[V]**

### 3.3 AppleSynopsysOTGDevice activation and reset

The iOS `AppleSynopsysOTGDevice` path was checked specifically for missing
bring-up steps. Its generic `setBusState(true)` path powers the PHY, waits 2 ms
when required, and calls `initController(true)`; the false path sets soft
disconnect, disables the controller and powers down the PHY. On activation,
`initController` calls the PHY provider's `setActive`, then `doCoreInit`,
initializes EP0 OUT and enables USB-reset/enum-done interrupts. `doCoreInit`
sets DCTL soft-disconnect, waits 4 ms, performs a DWC2 core soft reset, waits
for AHB idle, programs GAHBCFG/GUSBCFG and endpoint FIFOs, and flushes them.
Its `GRXFSIZ=0x21b` and
`GNPTXFSIZ=0x0010021b` are exactly the values in the Linux trace. **[V]**

On USB reset, iOS waits for the global IN/OUT NAK state and invokes
`AppleSynopsysOTGDevice_handleUSBReset`. That method ungates PCGCCTL when
needed, performs the generic USB-device reset cleanup, then calls
`startEp0Out`. The latter flushes all TX FIFOs, programs endpoint interrupt
masks, sets EP0's maximum packet to 64, clears the global NAKs and arms EP0 OUT
for setup. `enumDoneOccurred` only records the speed from DSTS. **[V]** None of
these functions calls AppleTriStar, changes a Lightning transport route,
enables `acc_pwr`/`acc_sw_en`, or performs the dock power handshake. **[V]**

Linux performs the same classes of operations on gadget activation and USB
reset. Its enum-done `enqueue_setup already queued???` means the request armed
before reset was still queued; the registers show EP0 OUT enabled, and the
forced-full-speed Mac succeeds through the same driver. **[V]** iOS does rearm
EP0 unconditionally from its reset handler, so forcing a Linux reset-time
reinitialization remains a possible later experiment, but the present trace
does not justify changing that path before testing the corrected cable-event
ordering. **[I]**

No `dxctrl`, ACC mux, VBUS gate, accessory-power or PHY change is supported by
this result, and there is no evidence-backed usb-host5 kernel change.
usb-host4 already has the required `CONFIG_USB_FUNCTIONFS=n` correction.
**[V]** The single next experiment is userspace-only: detect the live GetID
change, perform one clean UDC reconnect *before* the definitive GetID and power
handshake, then leave EP0 continuously armed across the adapter's retry.

## 4. Linux facade

`linux-a9-iap-roleswitch` leaves the existing ACM/ECM Mac-tether profile in
`g1/configs/c.1` and adds a temporary `g1/configs/c.2` named `iPod USB
Interface`, containing only `ffs.iap`. This matches the iUI configuration value
and also covers older non-audio Apple devices, whose iUI HID function was
interface 0. **[E]** The J71t function is interface 3 behind its audio
interfaces, but recreating those audio functions is not necessary for a
device-recipient request and would add unrelated failure modes. **[I]** The
role request itself contains protocol masks, not an interface-number check,
and FunctionFS remaps its virtual interface/endpoint numbers. **[V]** Adapter
tolerance of this standards-compatible HID-only iUI profile is the device test
**[I]**.

The facade deliberately leaves the gadget's global identity at the existing
`05ac:4142` and does not overwrite its strings. Changing the device-wide PID to
`12ab` would also change configuration 1 and has already been observed to stop
the unapproved Mac tether from configuring; a FunctionFS function cannot give
configuration 2 a separate device descriptor. **[V]** The Camera Adapter may
key recognition on the iUI configuration and HID interface alone, but whether
it also requires `12ab` is part of the device test. **[I]** Configuration 2 is
self-powered with zero requested bus power, keeping configuration 1 preferred
by ordinary hosts while preserving iOS's device-wide `Attributes=0xc0` and
`MaxPower=-1` intent. **[I]**

The daemon uses `FUNCTIONFS_ALL_CTRL_RECIP`, serves the appropriate HID/report
descriptor, logs every setup packet, consumes and logs `SET_REPORT`, implements
`0x51`/`0x53`, and stalls everything else. It ACKs `0x51` before waiting 50 ms
and calling the helper's shared gate/role/revert implementation. Listening has
no plug-in timeout; the duration argument begins only after an accepted role
switch.

The first device run also exposed two lifecycle assumptions. usb-host4 disables
the legacy `CONFIG_USB_FUNCTIONFS`/`g_ffs` gadget while retaining
`CONFIG_USB_CONFIGFS_F_FS`; otherwise `g_ffs` consumes f_fs's single device
slot and `mkdir functions/ffs.iap` returns `EBUSY`. **[V]** The daemon now
discovers the sole UDC when the configfs `UDC` attribute is empty, always
rebinds configuration 1 in device mode, and removes the temporary link,
function, mount, string and configuration on stop. The link target remains
absolute because configfs resolves a relative target from the caller's working
directory. **[V]**

With `--identify`, the helper watches the Tristar power-supply attachment state
and also performs a live GetID every two seconds while VBUS remains asserted,
the USB role remains device, and the Camera Adapter has not yet been found.
The latter detects Mac-to-adapter replacement even when the power event and
UDC disconnect are both missed. Polling stops after the adapter ID so it cannot
add sideband traffic to the enumeration under test. It does not create a timed
owner window. On a VBUS edge, owner request, or changed ID, the listener first
performs one clean 100 ms UDC reconnect, then records the definitive GetID and
optionally runs the power handshake only for this Camera Adapter. It leaves
EP0 connected after that sideband sequence. `listen-identify` queues the same
clean sequence explicitly. These actions remain in device mode and do not
impose a plug-in deadline.

## Sources

> Private graphics-dependent excerpt omitted from this source snapshot.

