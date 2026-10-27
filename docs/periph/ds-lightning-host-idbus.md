# Lightning host mode: how iOS identifies an accessory over Tristar (ID-bus reconstruction)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-26. Scope: the missing half of the camera-adapter host-mode work — what
iOS's **Tristar (CBTL1610) driver actually does over I²C** to identify a
Lightning accessory (the ID-bus / "AIDBus" master), and how the identification
connects to the already-reconstructed USB-host switch. Offline only: no device
access, no register writes. This file answers the three questions posed after
the 2026-10-26 hardware failure (`sig=se0`, no enumeration under any power
gate — [usb-host.md](usb-host.md) last section), and drafts the Linux driver
change plan.

Sources: the shared Ghidra kernelcache analysis (all addresses below),
`build/firmware/private/IOService.txt` + `IODeviceTree.txt`, `docs/adt/j71t.txt`,
the extracted `build/periph/round3-kexts/{AppleTriStar,IOAccessoryManager}.macho`
(objdump cross-checks), and the protocol reference
[lightning-idbus.md](lightning-idbus.md) (external-source half).

**Tags.** **[V]** verified in code/dumps (with function addresses).
**[I]** inferred. **[E]** external claim (from lightning-idbus.md, kept only
where code has not superseded it). Untagged = [V] from this round unless noted.

Everything below was renamed/commented in the shared Ghidra project and saved
(`scripts/ghidra-mcp.sh save`); see §11 for the rename list.

---

## 1. Answer in one page

1. **The object model.** `tristar@1A` (AppleARMIICDevice) →
   **`Port-Lightning@101`** = `AppleTriStarBuiltIn` (IOName `AppleTriStar2`,
   `TriStarICClass = AppleCBTL1610`; base class **`IOAccessoryDock9Pin`** is
   *defined in the IOAccessoryManager kext*) → its child **`AppleCBTL1610`**
   (the IC/ID-bus engine, base class `AppleTriStarIC`, also IOAccessoryManager
   kext) plus an `AIDBus` `IOPortTransportState` child. The **port** owns the
   register-access slots; the **IC** owns the ID-bus machinery and points back
   to the port (`port+0x1098 = IC`, `IC+0xd8 = port`).
2. **The SoC never speaks the 1-Wire-like ID-bus.** It drives **Tristar's ID-bus
   engine through I²C registers**: a **TX command window anchored at 0x60**
   (frame written at `0x60−txLen` … **0x60** with the payload **reversed**, GO
   byte at **0x61**), a separate **RX window at reg 0x20–0x27** (forward
   `[0x75, ID×6, CRC]`), a shared control register **0x05** (VBUS sink bit 0,
   reorient bit 1, BREAK bit 2, bus reset bit 3), an event register **0x0d**, and
   mux registers **0x01 (lanes "dxctrl") / 0x02 (ACC pins) / 0x03 / 0x17 (AID
   switch config)**. Tristar clocks the wire words itself.
   *(Correction of the first draft of this file: the `0x20`-payload /
   `0x40`/`0x41`-length transfer sequence belongs to a different IC
   generation's vtable, not our CBTL1610 — writing TX at 0x20 is actively
   harmful, see §5.4.)*
3. **GetID is a kernel-side, driver-initiated exchange.** The CPU writes the
   request `74 00 02` (HOSTID = `IOAccessoryManagerSystemID` = 2 on this unit)
   **reversed into the 0x5D–0x60 window plus the length/GO bytes and kicks
   reg 0x61**; the accessory's 8-byte response `0x75 + 6-byte ID + CRC8` lands
   **forward at 0x20–0x27**, and the driver re-reads it at any time via
   `getIdBusData`. This is exactly the piece our Linux trial performed wrong
   (right idea, wrong register window — §5.4).
4. **The 6-byte digital ID is a 16-field capability bitfield**, decoded and
   logged by `IOAccessoryManagerAID::handleDigitalID`
   (`fffffff005e34408`): byte 0 = `ACCx[7:6] DB[5] USB[4:3] MB[2] UART[1:0]`,
   byte 1 = `PWR[7:4] HV[3:2] AI[1] AU[0]`, byte 2 = `PU[7] BP[6] CC[3] BD[0]`,
   byte 3 = `PO[7] HS[6:4] HPM[2:0]`. This **supersedes** the THS7383-era
   ACCx/Dx nibble tables in lightning-idbus.md §5 (they were [E] and do not
   match this generation's decode).
5. **Power handshake.** VBUS-gated: send `0x72` (expect `0x73`+4), then 3-byte
   `0x70` frames `{70, flags, echo}` (flags bit 3 set on the retry path) — not
   the article's "three 0x70 with 0x80 data" ([E] corrected, see §6).
6. **USB host selection is *not* a direct ID-bus register effect.** After the ID
   is decoded and published (`IOAccessoryDigitalID`), host mode still comes from
   one of the three already-known `configureUSBMode(3)` entries: the
   **IapOverUsbHid vendor request 0x51** (the camera adapter enumerates the
   iPad as a device first), the **CHRG-type-5 fallback**, or an external
   message. What the ID provides is: the accessory is *known and powered*
   (power handshake done), USB lanes routed by Tristar per the accessory's
   declared ID, and the USB2 transport active — **without which none of the
   three paths can happen**. That matches today's `sig=se0`.
7. **The camera adapter's own ID is not in the binary.** No accessory-ID table
   exists in the kernelcache; the only hardcoded ID match found is a charger
   override for `(ID & 0xffffff) == 0x81f218` ("P275",
   `fffffff005df3b6c`). The adapter's ID must be captured on iOS →
   [lightning-ios-capture-plan.md](lightning-ios-capture-plan.md).
8. **Linux plan (§9):** a bounded, reversible extension of
   `apple_tristar_power.c`: a manual `identify` trigger that performs the
   GetID register sequence, decodes and publishes the ID, and (only after a
   captured camera-adapter ID exists) matches it and arms the existing host
   path. No EEPROM/OTP/ESN access, no persistent state, nothing but I²C
   register-pointer + volatile register writes.

---

## 2. Class architecture (verified)

Live object tree (`build/firmware/private/IOService.txt` ~7620–7790) [V]:

```
i2c0@A110000 / AppleS5L8940XI2CController
  └─ tristar@1A            <class AppleARMIICDevice>        compatible "tristar,cbtl1610"
      └─ Port-Lightning@101 <class AppleTriStarBuiltIn>     IOName "AppleTriStar2",
      │                                                      TriStarICClass "AppleCBTL1610",
      │                                                      TransportsProvisioned ("USB2","AIDBus"),
      │                                                      IOAccessoryDigitalID <100c00000000>,
      │                                                      IOAccessoryPowerHandshake = Yes
      │   ├─ AppleCBTL1610   <class AppleCBTL1610>            "TriStar REV" = 138 (0x8a)
      │   └─ AIDBus         <class IOPortTransportState>     IOAccessoryIDBusTransportMaxTransmit = 64
                                                           …MaxReceive = 63
```

Kext boundaries (from `build/periph/round3-kexts/*` Mach-O segments) [V]:

| Kext | `__TEXT_EXEC` range | Classes defined there |
|---|---|---|
| **IOAccessoryManager** | `fffffff005ddd000`–`5e5b240` | `IOAccessoryManagerAID` (manager), `IOAccessoryIDBusTransport` (command queue/protocol — source path `IOAccessoryIDBusTransport.cpp`), **`IOAccessoryDock9Pin`** (the *port base class*, instance size 0x1098, classInitialize `fffffff005e52824`), `AppleTriStarIC` (the *IC base class*) |
| **AppleTriStar** | `fffffff005fe4000`–`5ff6528` | `AppleTriStarBuiltIn` (port; vtable `fffffff006e52150`), `AppleCBTL1610` (IC; vtable `fffffff006e52be0`), AppleCBTL1612/1614/Kraken/Hydra siblings (shared code logs say "AppleCBTL1614::" even in this build) |

Key cross-kext links [V]:

- Port vtable slots: `+0x858` read register (`fffffff005fead50`), `+0x850`
  write register (`fffffff005fead10`) — the wrappers over AppleARMIICDevice
  (already documented in [tristar-recon.md](tristar-recon.md)).
- `AppleCBTL1610::start` = `fffffff005fef978`: reads DT `always-jtag-only`,
  `has-ana0`, `has-ana1`; creates the "ana" transfer controllers and — **J71t
  has none of these three properties** (checked in `docs/adt/j71t.txt`
  2408–2416) — so the IC stays in **mode 0** (no `changeAidSwitchConfig`
  at boot, reg 0x17 untouched). Default post-switch settle 10 ms at
  `+0x194`, overridable by boot-arg `ts-idbs-delay` (never set on us).
- Dock capabilities: `AppleTriStarBuiltIn::readDockCapabilities`
  (`fffffff005fe7aa0`) reads the dock node (`dock: 132` →
  `compatible "dock,9pin"`, `supports-trm = <nil>`,
  `supports-acc_sleep_pwr`, `primary-port-id 1`, `legacy-usb091216-override
  <nil>` — [V] `docs/adt/j71t.txt` 3254–3263) and caches the dock type at
  port `+0x11ac` (1 = not "dock,orion"; `|0x2000` if `supports-trm`).
  `getDockType` = `fffffff005fead98`.

---

## 3. Tristar CBTL1610 register map (this round's reconstruction)

All via the port's register slots (1-byte subaddress + N data bytes, the
STOP-separated pattern from tristar-recon.md; the FIFO window uses the
multi-byte form of the same slots).

| Reg | Meaning | Evidence (function) |
|---|---|---|
| **0x01** | **Lane-pair mux ("dxctrl")**: two nibbles (per lane pair), code values `0xC`/`0xD` seen. Cached at IC `+0x100` | `AppleCBTL1610_setDxControlRegister` `fffffff005ff5ed0` — selectors 2/3/4/5 → `low\|=0x0C`, `high\|=0xC0`, `low\|=0x0D`, `high\|=0xD0`; logs `write(dxctrl:0x%x)` / `reset(dxctrl:0x%x)`; 5 ms settle; reached from IC vtable `+0x630` (`5fef718`→ slot at `6e54f88`) |
| **0x02** | **ACC-pin mux**: two nibbles (ACC1/ACC2). Cached at IC `+0x101` | `AppleCBTL1610_setACCMuxRegister` `fffffff005fee258` (IC slot `+0x628`): cmd 0 → `(cached & 0xf0) \| 0x0C`, cmd 1 → `(cached & 0x0f) \| 0xC0`, cmd 0xffff → notify only; 5 ms settle; port notify slots `+0x830/+0x838` |
| **0x03** | Mux/power direction control (partial) | `AppleCBTL1610_writeMuxPowerRegister` `fffffff005ff5e10` (IC slot `+0x638`): under `state&7==1 \|\| state&0x70==0x10` and selector 4: writes **3** (or **1** if `state&7==1`), **0** when third arg 0. Exact semantics [I] |
| **0x05** | **ID-bus + port control**: bit 0 VBUS sink, bit 1 reorient, bit 2 BREAK, bit 3 ID-bus reset-hold. Cached at IC `+0x110` | bit 0: `setVBUSSink` `fffffff005ff5214` [V, known]; bit 1: `AppleCBTL1610_reorient_reg5_write_02` `fffffff005ff5418` (writes literal `0x02`); bit 2: `AppleCBTL1610_sendIDBusBreak_reg5bit2` `fffffff005ff583c` (`cached \| 4`, then `IODelay(20 µs)` — the 12–16 µs BREAK word); bit 3: `AppleCBTL1610_setIDBusResetHold_reg5bit3` `fffffff005ff5984` (IC slot `+0x5b8`), pulsed by `fffffff005ff5930` (set → `IOSleep(500)` → clear). While the reset flag (IC `+0x120`) is set, every ID-bus op refuses with `0xe00002d5` and logs *"… because ID_BUS is in reset"* |
| **0x0a** | **Pin-fault status**: `{bit0,bit3}` ACC1/ACC2, bit 1 DP1/DN1, bit 2 DP2/DN2. Read, report, **write the read value back to clear** | event-handler bit 7 path (`5fee47c`/`5feff40`); names at `5.4cda8a..a3` = `"ACC1","ACC2","DP1","DN1","DP2","DN2"`; fault flags at IC `+0x146/+0x144/+0x145`. Matches the IOReport `LightningPort*FaultCount` channels [V] |
| **0x0c** | VBUS/orientation (known) | `(v&3)==3` strict VBUS, bit 7 present, bit 6 orientation 1/2 — [tristar-recon.md](tristar-recon.md) |
| **0x0d** | **Event/status — full decode in §4** | `AppleCBTL1610_handleInterruptEvent_reg0x0d` `fffffff005fee47c` (our IC vtable slot `+0x5c0`) |
| **0x0e** | **Accessory-detect latch/mask**: `0xFF` = mask all, `0x01`/`0x00` = enable (selector stored at IC `+0x135`, consulted via the `+0x134` wait flag) | `AppleCBTL1610_setAccessoryDetectLatch_reg0x0e` `fffffff005fedb8c` (IC slot `+0x5c8`) |
| **0x0f** | Revision (`0x8a`) | known |
| **0x17** | **AID switch config**: bits 2/3 = "ana0"/"ana1" alternate channels (mode 1/2); RMW + verify + settle (10 ms default) | `AppleCBTL1614_changeAidSwitchConfig` `fffffff005fefd34`, helpers `5ff0c2c` (RMW bits 2/3) and `5ff0cc8` (verify), `_handleAidSwitchConfigChanged` `fffffff005ff0654` (+ block `5ff0d44`); waits ≤ 1 s for the `kAidBusSlaveACC` lock byte (IC `+0x190`). **Unused on J71t** (no `has-ana0/1`) |
| **0x18** | ANAX wake status, bit 0 → "EVENT2_ANAX_WAKE" (UART debug mux) | event-handler tails of `5fee47c`/`5feff40` |
| **0x20** | **RX window ONLY** (read): the response frame lands here **forward** — GetID: `[0x75, ID0..ID5, CRC8]` at 0x20–0x27. Generic commands: `expected+1` bytes. **Never write this window** on the CBTL1610 — a TX write clobbers the retained response's leading bytes (§5.4, hardware-proven) | RX reads: event handler `5fee47c` (`mov w1, #0x20`, `expected+1` bytes — raw objdump verified); `getIdBusData` `5feddf8` (8 bytes at 0x20 + orientation at 0x0c, verify `0x75`+CRC) |
| **0x5D–0x61** | **TX command window** (write), anchored so the length byte is always at **0x60**: block write of `txLen+1` bytes at **`0x60−txLen`** = `[payload reversed][rxlen byte]`, then the **GO byte at 0x61** = `((txLen−1)<<1)|1`. rxlen byte = `(rxLen−1)<<1`, plus `|0x80` if IC `+0x149` is set. On the J71t dock path the write is followed by a **read-back memcmp** of the same `txLen+1` bytes (abort `0xe00002ca` on *"readback mismatch"*) and a separate 0x61 GO write; other dock types do one `txLen+2` block write including GO | `AppleCBTL1610_sendIDBusCommand_frame_0x5D_go_0x61` `fffffff005feee8c` (IC slot `+0x690`, via `5fe5964` slot `+0x588`); `AppleCBTL1610_prepareIDBusCommand_reg0x60_eq_1` `5fef218` writes **reg 0x60 = 0x01** before each command (slot `+0x698`; [I] abort/clear pending engine command) |

**Generation note [V, hardware-confirmed]:** the *other* transfer
implementation, `AppleCBTL1610_startIDBusTransfer_regs_0x20_0x40_0x41`
(`fffffff005ff56a4` — payload at 0x20, `0x41 = 2*rxLen−2`, `0x40 = 2*txLen−1`),
sits in a **different generation's vtable** (the ~`6e54958`-based one, beside
the older event handler `5ff54e8` whose RX is read from `0x40−len` and
byte-reversed). It is **not** reachable from `AppleCBTL1610` (our vtable
`6e52be0`). Our first Linux identify attempt used its register layout — hence
the no-op/clobber behavior on the real CBTL1610.

The `2× − 1/− 2` length scaling [I]: the FIFO is addressed in half-byte
units; Tristar's engine appends the CRC and the framing BREAK words itself —
the kernel never puts a CRC byte in the TX buffer (the CmdID buffer is 3
bytes: type + 2 HOSTID bytes; §5) but the RX frame read back *includes* the
accessory's CRC byte (§4).

---

## 4. The attach/identify sequence, exactly

`AppleCBTL1610::handleInterruptEvent` (`fffffff005fee47c`, vtable slot
`+0x5c0`; our GPIO 72 interrupt / poll path) reads **reg 0x0d** and dispatches:

| 0x0d bit | Name/meaning | Action in code |
|---|---|---|
| `0x01` | accessory-detect | `AppleTriStarIC_handleAccessoryDetectEvent` `5fe5b6c` → port slot `+0x868` (dock `handleAccessoryDetect`) |
| `0x02` | **IDBUS WAKE** (slave-initiated re-enumeration) | `AppleTriStarIC_handleIDBusWakeEvent` `5fe586c` — sets IC `+0xfe`, notifies manager (`thunk_5e3b4b4`). Log *"EVENT_IDBUS_WAKE"* (in the 1614-gen twin `5feff40`) |
| `0x04` | **ID-bus reset / detach** | clears cached mux regs (IC `+0x100/+0x101`), reg-5 cache (`+0x110`), expected RX (`+0x118`), fault state; then `AppleTriStarIC_restartIDBusAfterReset` `5fe57ec` (clears the "identified" flag via port slot `+0x618`, re-arms). *"Accessory detached: _handleAidSwitchConfigChanged"* in `5feff40` |
| `0x08` | **EVENT_DIGITAL_ID** | `AppleTriStarIC_handleDigitalIDEvent_getIdBusData` `5fe58a0` → §5 |
| `0x18` (bits 3+4 — **hardware-observed for GetID**) | **GetID/digital-ID response received** | same handling via the bit-3 path: `getIdBusData` reads/verifies the frame at 0x20 — no separate RX read needed |
| `0x10` (with bit 3 clear, i.e. `(0x0d & 0x38) == 0x10`) | **transfer complete** (generic commands) | `len = IC+0x138` (1..64); read `len+1` bytes from **reg 0x20**; **CRC8** over first `len` bytes (via `idbus_crc8` thunk `5ff6348` → `5e39744`) must equal byte `len`; then `AppleTriStarIC_completeTransferToManager` `5fe516c(this, 0, buf)` |
| `0x20` | transfer error | `5fe516c(this, 0xe00002c6, 0)` |
| `0x40` | **timeout waiting for digital ID** | sets the wait flag (`+0x134 = 1`), re-latch (slot `+0x5c8`, reg 0x0e); and **if VBUS is present** (slot `+0x5f8` = `5ff498c`) → **`reorient`**: slot `+0x618` = `5ff5418` writes **reg 0x05 = 0x02** (try the other orientation) |
| `0x80` | pin fault | read **reg 0x0a**, report ACC1/ACC2 / DP1/DN1 / DP2/DN2, **write the value back** (clear), port slot `+0x418` sync |

Completion plumbing (IC → manager) [V]: `5fe516c` → (not terminating)
`IOAccessoryIDBusTransport::completeTransfer` `5e397a8` →
`completeTransferInternal` `5e3af70` (memcpy to the transfer's rxData, BPF tap
`5e2a640` `tapCommand` — an **IDBus sniffer hook exists**
(`_bpf_tap_in`, classes `AIDSnifferInterface`/`IOAccessoryIDBusUserClient`
present but not instantiated in our dump)) → `completeTransferCallback`
thread `5e3b384` → the queued command's completion. The queue itself:
`queueTransfer` `5e399b4` (slot `+0x558`) → `transferDataGated` `5e39b88`
(matching expected responses, transport selection) → the IC register writes.

**Ordering at a plug-in** (synthesized; each step [V] as cited):

1. Tristar detects the accessory on an ID pin and orientation (reg `0x0c`
   bit 7/6), raises 0x0d events autonomously — no SoC help needed for
   detection/orientation.
2. The IC/manager runs the **GetID (0x74)** exchange (§5). [The article's claim
   that a HiFive gates PWR to ~10–15 mA until the 0x74 is processed stays [E];
   today's `sig=se0` is consistent with the kernel-side exchange never
   happening under Linux.]
3. The response is decoded and published (§5); Tristar routes lanes per the
   accessory's declared capability bits (auto-routing [E]+[I]; the driver-side
   regs 0x01/0x02/0x03 exist for overrides such as SWD probes — J71t never
   writes them in the normal path).
4. The power handshake (§6) gates charging / accessory power.
5. With USB lanes routed and the USB2 transport active, the iPad enumerates as
   a USB device; the camera adapter sends **vendor request 0x51** on
   `IapOverUsbHid` → 50 ms → `configureUSBMode(3)` → the arbitrator publishes
   EHCI/OHCI and the PHY flips (`USB_CTL` bit 0, host trims) — all already [V]
   in [ds-usb-host.md](ds-usb-host.md) §3.1–3.4.

---

## 5. GetID and the 6-byte digital ID

### 5.1 The exchange [V + hardware-verified]

`AppleTriStarIC_sendCmdID_0x74_with_HOSTID` (`fffffff005fe4814`):

- Reads HOSTID from the **`IOAccessoryManagerSystemID`** property (2 on this
  unit — matching the article's captured `74 00 02`).
- Anti-spam: refuses if < 100 ms since the last CmdID
  (*"prevent sending CmdID since %llu us has elapsed"* — the
  `idCommandTooFast` statistic; counters at IC `+0x124…`).
- Queues the transfer via the IC's slot `+0x558` with **buffer = 3 bytes
  `{0x74, HOSTID lo, HOSTID hi}`** (no CRC byte — Tristar appends it),
  **expected response length 7** (`0x75` + 6 ID bytes), 100 ms timeout,
  itself as completion → retry loop.
- **First calls `prepare`** (IC slot `+0x698` = `5fef218`): **write reg
  0x60 = 0x01** ([I]: abort/clear any pending engine command), expected-length
  cache `+0x138 = −1`.
- **Register level** (via `AppleCBTL1610_startTransfer_GetID_entry_slot588`
  `5fe5964` → `AppleCBTL1610_sendIDBusCommand_frame_0x5D_go_0x61` `5feee8c`;
  the previous draft of this file wrongly attributed the TX to `5ff56a4`'s
  0x20/0x41/0x40 layout — see the generation note in §3):
  1. cache expected rxLen at IC `+0x138`;
  2. **I²C block write of 4 bytes at reg 0x5D** (`0x60−3`):
     **`02 00 74 0c`** — the payload *reversed* (`{74,00,02}` → `02,00,74`)
     followed by the rxlen byte **`(7−1)<<1 = 0x0c`** at reg 0x60;
  3. **I²C block read of 4 bytes at reg 0x5D and memcmp** — the
     *"readback mismatch: [%02x] txDataLength=%d, rxDataLength=%d"* guard
     (abort `0xe00002ca`, no GO);
  4. **I²C write reg 0x61 = 0x05** — the GO byte `((3−1)<<1)|1`.

Response handling: on the **`0x0d = 0x18`** event (bits 3+4 — EVENT_DIGITAL_ID
plus complete; hardware-confirmed) or `(0x0d & 0x38) == 0x10` for generic
commands, **read the RX window at reg 0x20** (`expected+1` bytes; GetID: 8).
`getIdBusData` (`5feddf8`, IC slot `+0x5e0`) re-reads the retained frame at
any time: **8 bytes at reg 0x20** (I²C: write subaddr 0x20 + STOP, read 8
bytes + STOP — the `i2c_smbus_read_i2c_block_data` shape) **+ 1 byte at reg
0x0c** (orientation); requires `frame[0] == 0x75` and
`CRC8(frame[0..6]) == frame[7]`; the **ID = frame[1..6]**. The frame stays
readable until the next transfer overwrites the window.

### 5.4 Hardware validation and the corrected model (2026-10-26, `a9/usb-host2`)

The first identify trial (kernel `9977e33ee6`, implementing this file's
original §9) proved the *event/RX/CRC* half right and the *TX window* wrong:

- **The RX window is 0x20–0x27, forward.** After one reorient
  (`reg 0x05 = 0x02`), both the Mac cable and the camera adapter produced
  `0x0d = 0x18` and the byte at **reg 0x27 read `0x66` for the Mac cable —
  exactly `crc8(75 10 0c 00 00 00 00)`** (recomputed offline; also
  `crc8(74 00 02) = 0x1f` matches the article's TX vector). The adapter read
  `0xd6` at 0x27.
- **Why bytes 0–2 showed `74 00 02` instead of `75 xx xx`:** our TX write of
  `{74,00,02}` went to reg 0x20 — *the RX window* — and **clobbered the first
  three bytes of the already-received response** (`75→74`, ID byte 0 → `00`,
  ID byte 1 → `02`). Bytes 3–6 (`00 00 00 00`) and byte 7 (the CRC) were
  untouched — for the Mac cable this reconstructs the true frame
  `75 10 0c 00 00 00 00 66` bit-for-bit. The pre-boot leftover frame had the
  same clobber shape (an earlier TX write into 0x20 — iBoot-era convention —
  after a completed exchange).
- **Why the no-reorient GetID never completed:** the TX frame, rxlen and GO
  went to 0x20/0x41/0x40 — the register layout of a *different IC
  generation*'s transfer function (`5ff56a4`), inert on the CBTL1610. No
  command ever reached the engine, and the engine was idle since boot.
  The **reorient is what re-armed the autonomous ID exchange** — iOS's own
  digital-ID-timeout fallback — which then ran the whole 0x74 exchange by
  itself (response + `0x18`) with no valid CPU command involved.
- **Consequence for the adapter's ID:** its response was
  `75 [ID0] [ID1] 00 00 00 00 d6` with ID0/ID1 clobbered. The CRC pins the
  pair: `crc8(75 ID0 ID1 00 00 00 00) = 0xd6` has **exactly 256 solutions —
  one unique ID1 per ID0** (machine-computed and CRC-validated; full table in
  [`logs/usb-host2-20261026/analysis.txt`](../release/OMISSIONS.md)).
  Plausible members: `10 ae` (USB=2 like the Mac cable), `30 8d` (DB=1,
  USB=2), `18 6c` / `38 4f` (USB=3). The corrected driver reads the full
  frame and settles it.

### 5.2 The 16-field decode [V] — correction to lightning-idbus.md §5

`IOAccessoryManagerAID::handleDigitalID` (`fffffff005e34408`) logs and
forwards the ID; the field extraction is in its argument setup
(`5e34520–5e34568`, disassembly):

| Byte | bits 7:6 | bits 5:4 | bits 3:2 | bits 1:0 | (single-bit fields) |
|---|---|---|---|---|---|
| 0 | **ACCx** (2) | — | — | **UART** (2) | bit 5 **DB**, bits 4:3 **USB** (2), bit 2 **MB** |
| 1 | **PWR** (4) | — | **HV** (2) | — | bit 1 **AI**, bit 0 **AU** |
| 2 | **PU** (bit 7) | **BP** (bit 6) | **CC** (bit 3) | **BD** (bit 0) | |
| 3 | **PO** (bit 7) | **HS** (bits 6:4) | — | **HPM** (bits 2:0) | |

The exact log format: `USB=%x ACCx=%x DB=%x MB=%x UART=%x PWR=%x HV=%x AI=%x
AU=%x PU=%x BP=%x PO=%x CC=%x BD=%x HS=%x HPM=%x`.

Re-decoding the known IDs from lightning-idbus.md §5 with this table:

| Accessory | ID | Decoded (this table) |
|---|---|---|
| Mac cable (**ours, live [V]**) | `10 0C 00 00 00 00` | ACCx=0, DB=0, **USB=2**, MB=0, UART=0, PWR=0, HV=3, AI=0, AU=0 |
| DCSD | `20 00 00 00 00 00` | ACCx=0, **DB=1**, USB=0, UART=0 |
| KongSWD+Astris | `A0 00 00 00 00 00` | **ACCx=2**, DB=1, USB=0 |
| EarPods | `04 F1 00 00 00 00` | MB=1, PWR=0xF, AU=1, USB=0 |
| Haywire HDMI | `0B F0 00 00 00 00` | UART=3, USB=1, PWR=0xF |

Field *semantics* beyond the names are [I] — e.g. `USB=2` for a plain
device-mode cable suggests a role/capability code (0/1/2/…), not the old
2-bit Dx lane table; the `USB` value of the camera adapter is exactly the
unknown to capture. Publishing: `IOAccessoryManagerAID_setAccessoryDigitalID`
(`5e33c5c`, property **`IOAccessoryDigitalID`**, OSData 6 bytes; removed on
disconnect); getter `IOAccessoryDock9Pin_getDigitalID` (`5e33f40`, slot
`+0x5a8`). `IOAccessoryDock9Pin::handleDigitalID` base (`5e4e2fc`, port slot
`+0x700`) additionally handles orientation changes, kicks a 60 s
"allowedFeaturesAvailable" watchdog, and schedules follow-up processing
(100 ms for dock-type-nibble-0 vs 2000 ms otherwise — the dock type, not the
ID, selects that timing).

The manager side reacts to specific IDs only sparsely: the one hardcoded match
is `(digitalID & 0xffffff) == 0x81f218` → "P275" → cap
`kIOAccessoryPowerModeLow` at 100 mA (`fffffff005df3b6c`), and
`shouldOverridePowerHandshake` (`5e4df18`) matches IDs with
`byte1==0xfe && (byte2&0xc0)==0x80 && …` → *"accessory needs PH override"*.
There is **no camera-adapter ID in the binary** → capture (Q2).

---

## 6. Power handshake (0x72/0x70) — corrected sequence

The state machine sits in an undefined-function region of IOAccessoryManager
(disassembled at `fffffff005e4f5d8`…`5e4fa40`; the response handler is
`IOAccessoryDock9Pin::handleIDBusCommand` at `5e4f168`, switch with jump table
`5.476cd2`):

1. **Gated on VBUS**: port slot `+0x780` (getVBUSDetect). No VBUS →
   *"no VBUS detected when starting powerhandshake"*, sleep 100 ms, state 3,
   retry (debounce item tagged
   `IOAccessoryDock9Pin::handleIDBusCommandForPowerHandshakeDebounce`, symbol
   created in `5e52824`).
2. **Send `0x72`** (1 byte, via the IC queueTransfer slot `+0x558`,
   expected response 5 = `0x73` + 4 data). On the `0x73` response
   (`5e4f418` case, checks last-response-type `0x71`/`0x73` at IC-manager
   `+0xea0`): sets `+0xf66=1`, copies response bytes into `+0xf67/+0xf68/
   +0xf69`, logs *"setPowerHandshakeState(kPowerHandshakeLimit)"*, state 0x10.
3. **Send 3-byte `0x70` frames**: `{0x70, flags, echo}` where
   `flags = +0xf67 & ~8` (normal) or `+0xf67 | 8` (retry/verification path),
   `echo = +0xf68` — both derived from the last GetState response [I].
   Each queued with expected response 1 byte (`0x71`).
4. Failures log *"power handshake verification failed"* / *"handshake
   aborted"* and re-run through the MagicPullDown debounce
   (`IOAccessoryDock9Pin::handleIDBusCommandForMagicPullDown`).

**Corrections to lightning-idbus.md §7 [E]**: the kernel's sequence is
`0x72 → 0x73 → 0x70` frames; the "two 0x70 back to back, ~20 ms current
disable, third 0x70 with 0x80 data" was not found — the closest reality is a
3-byte 0x70 request whose *second* byte carries **bit 3 (0x08)** on the retry
path, and 100 ms sleeps on failure. The 20 ms figure also appears nowhere;
the 18 ms VBUS-sink settle is a different register. The published
`IOAccessoryPowerHandshake = Yes` property is the completion marker [V].

---

## 7. How host mode is selected (Q1 synthesis)

`configureUSBMode(3)` (gated entry `fffffff005df3d20` →
`IOAccessoryManager_configureUSBModeGated` `5df2228`) has exactly three
callers [V xrefs]:

1. **`IOAccessoryPortUSB` vendor request 0x51** — the adapter enumerates the
   iPad as a USB *device*, selects the `IapOverUsbHid` configuration, sends
   `0x40/0x51`; the handler arms the mode and a 50 ms timer calls
   `configureUSBMode(3)` (`6c3d61c`/`6c3d5c4`, documented in ds-usb-host §3.1).
   This is the camera-adapter path.
2. **`usb_connect_shim_handle_chrg`** (`5e369c4`): a CHRG message
   (`0x43485247`) with **type 5** → `configureUSBMode`-family calls
   (`5df3d20(port,1)` + `5df21bc(port,3)`). Logs
   *"chargerFunction: ilim=%dmA type=%x"*. Type 5 is the "USB host adapter"
   classification from `IOAccessoryDock9Pin::determinePowerSupply` — the
   fallback for adapters that don't speak the vendor request.
3. A message-driven thunk (`5e1fcbc`) — mode from a message argument.

So the ID-bus identification does not *itself* flip USB host; it makes the
port alive (transport provisioning, power handshake, lane routing) so that
path 1 or 2 can fire. The EHCI/OHCI publication (`updatePublishedNubs`,
criteria 3/4) and the electrical switch (`enableHostMode`, `USB_CTL` bit 0,
host trims) are unchanged from ds-usb-host §3.3–3.4 [V].

Accessory power [V ds-usb-host §3.5, unchanged]: PMU dock functions
`acc_pwr` (D2365 LDO5) / `acc_sw_en` / `acc_sleep_pwr`, plus the Tristar
reg-0x05-bit-0 VBUS-sink gate (18 ms settle). The dock type for J71t is 1
(`"dock,9pin"`), no TRM, no Orion.

---

## 8. What cannot be known statically (Q2)

- The camera adapter's 6-byte ID and its decoded `USB`/`ACCx`/… fields —
  **not in the kernelcache** (§5.2), not in any plist we have.
- Whether the adapter's HiFive requires the kernel-side 0x74/0x72/0x70 to
  power/route (the [E] 10–15 mA gate claim), or whether Tristar's autonomous
  exchange suffices — today's Linux `sig=se0` says *something* was missing;
  the capture will show which.
- Whether the adapter issues a WAKE before renegotiating after it has
  enumerated the iPad as a device (lightning-idbus §8 [I]).

→ The capture plan: **[lightning-ios-capture-plan.md](lightning-ios-capture-plan.md)**.

---

## 9. Linux driver change plan (Q3; no build yet)

### 9.1 Design point

The SoC's job is only the **I²C command side**; Tristar does wire timing,
BREAK words and CRC appending. So the minimal Linux addition is a
**GetID-capable, event-consuming extension of the existing
`apple_tristar_power.c` supplier** (patches 0040/0050 lineage,
`scripts/periph/kernel/`), plus a hook into the existing `a9/usb-host` role
path. Nothing else in the stack changes.

### 9.2 Register sequence to implement (corrected after the 2026-10-26 trial)

**Identify (manual `identify` trigger, or later on 0x0d bit 3):**

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

CRC8 (shared with the wire protocol): generator `0x8C` reflected, init `0xFF`,
LSB-first — implement the 8-line reference from lightning-idbus.md §5.2; test
vectors now **hardware-confirmed**: `74 00 02 → 1f` and
`75 10 0c 00 00 00 00 → 66` (the value observed at reg 0x27 with the Mac
cable attached).

I²C access pattern: the 1-byte registers keep the faithful
STOP-separated byte-pair from tristar-recon.md; the TX/RX windows need
**block** ops — `i2c_smbus_write_i2c_block_data(client, 0x5D, 4, …)` and
`i2c_smbus_read_i2c_block_data(client, 0x20, 8, …)` — matching the port's
register slots which take (reg, buf, len) [V; the wrappers `5fead10`/`5fead50`
dispatch provider op 2/1 with subaddress length 1]. Require
`I2C_FUNC_SMBUS_I2C_BLOCK` and fail closed without it.

**Event consumption (the poll loop already exists):** decode reg 0x0d bits as
§4: bit 2 (detach) clears the cached ID + mux caches; bit 3/0x10 run the RX
path; bit 0x40 with VBUS → the reorient hint; bit 0x80 → read reg 0x0a, log
the pin faults, **write the same value back** (never arbitrary bits). Reg
0x0e: leave alone in v1 (it is a mask iOS toggles around the exchange;
mis-timing it could suppress accessory-detect events).

**Arming host mode:** unchanged existing `a9/usb-host` path (role switch →
PHY host trims → `USB_CTL` bit 0 → EHCI/OHCI platform devices), plus the
documented accessory-power gates (`acc_pwr`/`acc_sw_en` regulators, then
Tristar reg 0x05 bit 0 with 18 ms settle). Once the captured camera-adapter
ID exists (§8), the driver matches `ID == <captured>` (or a module-parameter
allowlist) and auto-arms host; until then the arm stays manual
(`linux-a9-usb-host arm …` / sysfs role).

### 9.3 Surface for humans

- `debugfs apple-tristar/`: `digital_id` (hex), `digital_id_decoded` (the 16
  fields, names as iOS logs them), `reg_05`, `reg_0d` (last read), `reg_0a`.
- sysfs on the supplier: `identify` (write 1 → run 9.2, store the result),
  read-only `digital_id`.
- Keep the trial-log evidence requirement from usb-host.md: every identify
  run logs the raw frame + decode, so a future failure can be diffed against
  the captured iOS baseline.

### 9.4 Safety notes (hard rules)

1. **Nothing persistent.** Only volatile I²C registers are written. Never
   touch the ID-bus OTP family (0xE0–0xE5 requests), never ESN provisioning,
   never `ttrs.apple.com`, never syscfg/NVRAM/boot firmware (AGENTS.md).
2. **Reg 0x05 is shared** (VBUS sink / reorient / BREAK / bus reset):
   always read-modify-write the cached value under the supplier mutex; never
   clear bit 3 unless we set it (we never set it in v1); the reorient write
   uses iOS's literal `0x02` form only when bit 0 is clear, else `| 0x02`.
3. **Reg 0x0a write-back must echo the read value** (ack semantics).
4. **Never write reg 0x17** (J71t has no ana channels; iOS leaves it 0).
5. **Fail closed and stay out of the charger's way**: identification must not
   re-arm charging in host mode; the supplier's fail-closed read-error
   behavior is unchanged; GetID failure must not block the existing
   device-mode/charging paths.
6. **Manual first**: `identify` is a user-triggered, bounded (≤ 2 retries,
   ≤ 200 ms) operation; auto-on-event only after the captured ID confirms
   the decode. Auto-arm of host mode only behind the captured-ID match.
7. The BREAK bit (0x05 bit 2) and bus-reset bit are **not** used by the v1
   sequence — Tristar frames the transfer itself; adding manual BREAKs would
   be a second, riskier experiment.

### 9.5 Why this should fix `sig=se0` (and what if it doesn't)

The 2026-10-26 trial shifted the model: the adapter's HiFive *did* answer a
GetID (the `0x18` + response CRC at reg 0x27 after one reorient) — but the
CPU-side command never reached the engine (wrong TX window), so the exchange
that completed was the reorient-triggered *autonomous* one, and our TX write
then clobbered its frame. With the corrected window the exchange is fully
driver-driven and the ID is read intact. Whether routing/power then follow
(the [E] HiFive gate claim) is exactly what the next trial answers: if a valid
`75 …` frame is read and the lanes still show SE0 in host mode, the next
suspects are (i) the accessory-power gates (retry the `pmu`/`both` ladder
*after* a successful identify), (ii) the adapter's own power input, (iii) a
needed WAKE from the adapter after it enumerated the iPad as a device, and
(iv) the 0x72/0x70 handshake — all distinguishable from the debugfs decode.

### 9.6 Recovering normal USB after a reorient/identify session (no reboot)

Observed after the 2026-10-26 session: the Mac USB link stays "not attached"
even after replugging the cable in both orientations (`reg 0x05 = 0x00`,
`0x0d = 0x18`). The IC sits with an unconsumed digital-ID event and lane
routing never re-applied. iOS's own recovery levers, in the order to try
them (all volatile; always read reg 0x05 first and toggle only the named bit):

1. **Reorient again** — write `reg 0x05 = value | 0x02` (current value 0x00 →
   write `0x02`). This is iOS's *restart-after-detach* and
   *digital-ID-timeout* mechanism (the detach path `5fe57ec` and the event
   handler's timeout branch both land in `AppleCBTL1610_reorient_reg5_write_02`
   `5ff5418`). Wait ~20–50 ms, then poll `0x0d` and **read** the RX window at
   0x20 (read-only!) to consume the frame. A replug + reorient mirrors what
   iOS does on every detach/attach cycle.
2. **ID-bus reset pulse** — `AppleCBTL1610_pulseIDBusReset_500ms` (`5ff5930`
   via `5ff5984`): write `reg 0x05 = value | 0x08` (hold the engine in
   reset), `msleep(500)`, write `reg 0x05 = value & ~0x08`. On release the
   engine re-runs the whole attach from scratch (orientation, autonomous
   GetID, lane routing). This is the strongest un-wedger and the only
   sanctioned use of bit 3. While it is held, every ID-bus operation refuses
   (`0xe00002d5`) — issue no commands in between and never hold it longer
   than the pulse. Current reg 0x05 reads 0x00, so the pulse values are
   exactly `0x08` then `0x00`.
3. **Fault ack** — read `reg 0x0a`; if any of bits 0–3 are set (ACC1/ACC2 /
   DP1/DN1 / DP2/DN2 fault latches can keep the lane mux disabled),
   **write the read value back** (iOS's ack pattern in the event handler's
   0x0d bit-7 path). Never write any other value.
4. Leave `reg 0x0e` (detect latch) alone — iOS toggles it around its own
   exchanges; a mistimed write can mask accessory-detect events.

After each step, check `reg 0x0c` (bit 7 = accessory/orientation present,
`(v & 3) == 3` = VBUS) to see whether the IC re-detected the cable, and
whether the Mac sees the gadget again. With the corrected driver (no 0x20
writes; the event is consumed by a completed read) this wedge should not
recur.

---

## 10. Evidence and reproduction (function index)

All renamed in the shared Ghidra project (saved):

| Address (old name) | New name / meaning |
|---|---|
| `fffffff005ff56a4` | `AppleCBTL1610_startIDBusTransfer_regs_0x20_0x40_0x41` — **⚠ other generation's vtable, not our IC** (§3 generation note, hardware-confirmed) |
| `fffffff005feee8c` | **`AppleCBTL1610_sendIDBusCommand_frame_0x5D_go_0x61`** — the real CBTL1610 TX sequence (reversed payload at `0x60−txLen`, rxlen byte at 0x60, readback-verify, GO at 0x61) |
| `fffffff005fe5964` | **`AppleCBTL1610_startTransfer_GetID_entry_slot588`** — GetID special case (3/7/0x74), busy-flag gating, dock-type flag w4 |
| `fffffff005fef218` *(comment only)* | `AppleCBTL1610_prepareIDBusCommand_reg0x60_eq_1` — reg 0x60 = 0x01 before each command |
| `fffffff005fe5908` | `AppleCBTL1610_idBusPollTimer_resend_CmdID` — periodic poll: timeout previous (0xe00002eb), re-send CmdID or re-publish the retained ID |
| `fffffff005fe5ab4` | `AppleCBTL1610_startTransfer_skipNonGetID_whileBusy` — the `idCommandSkipped` counter path (port+0x10c0) |
| `fffffff005fead10` / `5fead50` | port register write/read wrappers: provider op 2/1, subaddress length 1, data length N (block-capable) |
| `fffffff005ff583c` | `AppleCBTL1610_sendIDBusBreak_reg5bit2` |
| `fffffff005ff5800` | `AppleCBTL1610_abortIDBusReceive_reg41` |
| `fffffff005ff5418` | `AppleCBTL1610_reorient_reg5_write_02` |
| `fffffff005ff5984` / `5ff5930` | ID-bus reset-hold bit / 500 ms pulse |
| `fffffff005ff5320` | `AppleCBTL1610_getVBUSSink_reg5bit0` |
| `fffffff005ff54e8` | older-IC event handler (RX from `0x40-len`, byte-reverse — not our class) |
| `fffffff005feff40` | 1614-generation event handler (same register map, richer) |
| `fffffff005fee47c` | **`AppleCBTL1610_handleInterruptEvent_reg0x0d`** (ours) |
| `fffffff005fe4814` | **`AppleTriStarIC_sendCmdID_0x74_with_HOSTID`** |
| `fffffff005fe516c` | `AppleTriStarIC_completeTransferToManager` |
| `fffffff005fe58a0` | `AppleTriStarIC_handleDigitalIDEvent_getIdBusData` |
| `fffffff005fe57ec` / `5fe586c` / `5fe5b6c` | restart-after-reset / IDBUS WAKE / accessory-detect |
| `fffffff005feddf8` *(undefined fn — comment only)* | `getIdBusData`: re-read 8 bytes reg 0x20 + reg 0x0c, verify 0x75+CRC |
| `fffffff005fedb8c` | `AppleCBTL1610_setAccessoryDetectLatch_reg0x0e` |
| `fffffff005fedbe8` | read 0x0d then 0x0a helper |
| `fffffff005fee258` | `AppleCBTL1610_setACCMuxRegister_reg0x02` |
| `fffffff005ff5ed0` | `AppleCBTL1610_setDxControlRegister_reg0x01_dxctrl` |
| `fffffff005ff5e10` | `AppleCBTL1610_writeMuxPowerRegister_reg0x03` |
| `fffffff005ff0a08` | `AppleCBTL1610_getIdCommandExpected_switchConfig` |
| `fffffff005fef978` | `AppleCBTL1610_start` (dock/ana init, `ts-idbs-delay`) |
| `fffffff005fefd34` / `5ff0654` / `5ff0c2c` / `5ff0cc8` | `changeAidSwitchConfig` family (reg 0x17) |
| `fffffff005ff0700` / `5ff0f48` | completeTransferController / stop-to-mode-0 |
| `fffffff005fe7aa0` / `5fead98` | dock capabilities reader / getDockType (`+0x11ac`) |
| `fffffff005fe52dc` *(comment only)* | port `handleIDBusCommand` dispatch (jump table `5.4cbd50`) |
| `fffffff005e39744` | **`idbus_crc8_poly_0x8c_init_0xff`** |
| `fffffff005e399b4` / `5e39b88` / `5e397a8` / `5e3af70` / `5e3b384` | IDBusTransport queue/complete chain |
| `fffffff005e2a640` | `tapCommand` (IDBus BPF tap) |
| `fffffff005e34408` / `5e33c5c` / `5e33f40` | manager handleDigitalID / set / get (property `IOAccessoryDigitalID`) |
| `fffffff005e4e2fc` / `5e4df18` | port handleDigitalID base / `shouldOverridePowerHandshake` |
| `fffffff005e4f168` *(comment only)* / `5e4f5d8` *(comment only)* | `handleIDBusCommand` switch / power-handshake state machine (0x72/0x70) |
| `fffffff005df3b6c` | P275 digital-ID match (`0x81f218`) |
| `fffffff005e369c4` | `usb_connect_shim_handle_chrg` (CHRG type 5 → host) |

Reproduction: `scripts/ghidra.py decompile_function_by_address address=…`
(shared server, already running); raw cross-checks
`objdump -d --start-address=… build/periph/round3-kexts/{AppleTriStar,IOAccessoryManager}.macho`;
live dump `build/firmware/private/IOService.txt` lines ~7620–7790 and ~13560
(power-source items incl. `IOAccessoryPowerSourceItemUSBHost` /
"determinePowerSupply").

## 11. Open questions

1. Reg 0x03 exact semantics (only partially decoded — §3).
2. HOSTID byte order in the 0x74 data (big-endian per the article's capture
   `74 00 02`; the code builds two bytes via two `+0xa0` calls whose order
   the decompiler lost — the corrected identify reads the response and
   settles it empirically).
3. Whether Tristar auto-routes lanes purely from the accessory-declared ID,
   or whether the driver must echo something into regs 0x01/0x02 for the
   camera adapter (J71t never writes them in the normal path — [I] not
   needed).
4. The camera adapter's ID0/ID1 — **pinned to 256 candidates by the observed
   CRC `0xd6`** (§5.4); the corrected driver's next read settles it. The
   `USB` field value set (2 = plain cable) follows from it.
5. Whether the adapter's power input (charger attached) changes its reported
   ID or handshake state — capture both configurations.
6. What exactly the reorient-armed *autonomous* exchange ran (the engine's
   own GetID with its own HOSTID?) versus what the CPU-side command
   contributes — the corrected driver can compare a reorient-only identify
   against a full-command identify.
7. The role of the prepare write `reg 0x60 = 0x01` ([I]: abort/clear pending
   command) — safe to mirror, semantics unconfirmed.

**Next concrete step:** re-run the identify with the corrected §9.2 TX window
(same kernel branch lineage, small delta: TX at 0x5D/0x60/0x61, read-only
0x20) — this reads the camera adapter's full ID (closing question 4) and
tells whether lane routing follows identification. The iOS capture
([lightning-ios-capture-plan.md](lightning-ios-capture-plan.md)) remains the
cross-check for the decode and the post-GetID sequence.
