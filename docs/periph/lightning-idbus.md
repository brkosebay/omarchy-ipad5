# Lightning IDBUS / accessory identification — protocol reference

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-26. Scope: what the Lightning port's accessory-identification bus
(Tristar ↔ accessory, Apple's "AIDBus", NXP's "IDBUS", community "SDQ")
actually is, transcribed into project documentation so the camera-adapter
host-mode work can use it. This file folds in one external source and
cross-checks it against our own saved device evidence. No device access was
performed for this file; no registers were touched.

This is the reference that [ds-usb-host.md](ds-usb-host.md) §3.6 deferred
("Reimplementing ID-bus identification is a much larger job") and that
[usb-host.md](usb-host.md) calls "the next task" when the manual role-switch
trial fails to enumerate an adapter.

## Provenance and tags

Primary external source: NyanSatan, *"Apple Lightning"*,
<https://nyansatan.github.io/lightning/> (written 2020-07-20; based on the
author's logic-analyzer captures plus a diagonal read of leaked AppleInternal
datasheets/schematics; credits there to @key2fr, @_L1ngL1ng_ and @spbdimka,
who published the IDBUS command-handler list). The author explicitly warns the
write-up may be partially wrong. Two of its images (port/connector pinouts and
the spbdimka command list) were fetched and OCR'd with tesseract to build the
tables below; transcribe no further claims from them than are written here.

Tags used in this file:

- **[E]** — external claim from the article/its datasheets, **not verified on
  our hardware**. Treat as a hypothesis with good grounding, not as fact.
- **[V]** — verified on our hardware or in our saved dumps (includes external
  claims that our own evidence independently corroborates).
- **[I]** — our inference, weaker than [V].

Everything untagged is [E].

## 1. Hardware model

- **Lightning** = 8-pin, orientation-agnostic digital connector. The male
  connector's two sides are **not** wired in the same order, so the host must
  detect cable orientation before routing anything; many accessories simply
  mirror their pinout instead. **[E]**
- **Tristar** = the IC in the device behind the Lightning port: a MUX plus the
  accessory-negotiation controller. It detects orientation, talks to the
  accessory over IDBUS, then routes internal interfaces (USB, UART, SWD/JTAG)
  onto the connector pins per the negotiated configuration. **[E]** for the
  general description; the routing semantics are in §5.
- **HiFive** = the slave chip inside Lightning accessories (male side), known
  as SN2025/BQ2025. Cable-class accessories have a real HiFive; SWD probes
  (Kong/Kanzi) emulate it with a microcontroller/FPGA. **[E]**
- Tristar/Hydra family **[E]**:

| Part | Generation |
|---|---|
| TI THS7383 | first gen (iPad mini 1, iPad 4) — the only variant whose datasheet publicly names "IDBUS" |
| NXP CBTL1608A1 | first gen (iPhone 5, iPod touch 5) |
| NXP CBTL1609A1 | first gen (iPod nano 7) |
| NXP CBTL1610Ax | second gen, since iPhone 5C/5S — "everything without wireless charging" |
| NXP CBTL1612Ax | Hydra, since iPhone 8/X — wireless-charging devices |

**Our unit is CBTL1610, revision 0x8a** (`tristar,cbtl1610` in the ADT, i2c0
0x1a; `TriStar REV = 138` in the live dump) — **[V]**, consistent with the
"second gen, no wireless charging" row. Everything Hydra-specific is
irrelevant to J71t.

## 2. Cross-checks against our own evidence

The article's claims line up with our saved ADT/IOService dumps
(`build/firmware/private/IODeviceTree.txt`, `IOService.txt`, taken with the Mac
cable attached in device mode) on every point we can test without new
hardware:

| Article claim | Our evidence | Tag |
|---|---|---|
| Tristar sits on I²C at 8-bit address 0x34 (write) / 0x35 (read) | 8-bit 0x34/0x35 ⇔ 7-bit **0x1a** — exactly our ADT i2c0 address ([tristar-recon.md](tristar-recon.md)) | **[V]** |
| 0x75 (GetID response) carries a 6-byte "digital ID"; a plain USB cable answers `10 0C 00 00 00 00` | live dump: `IOAccessoryDigitalID = <100c00000000>` — same format, same value class (Mac cable attached) | **[V]** |
| The protocol is called IDBUS (datasheet) | iOS calls the same transport **AIDBus**: `TransportsProvisioned/TransportsActive = ("USB2","AIDBus")`, `IOAccessoryIDBusState`, `IOAccessoryIDBusTransportMaxTransmit = 64`, `MaxReceive = 63` in the live dump | **[V]** (that they are the *same* bus is **[I]**, but see the next two rows) |
| Byte 0 of the ID contains `ACCx[1:0]` and `Dx[1:0]` mux configuration fields | Apple's own IOReport channels on the same object are named `LightningPortACCxFaultCount`, `LightningPortDx1FaultCount`, `LightningPortDx2FaultCount`, plus ACC1/ACC2/DP1/DN1/DP2/DN2 overcurrent counters — identical vocabulary | **[V]** |
| A request always starts with a BREAK word | Apple driver statistics counter `idCommandBreakPrevented` (alongside `idCommandTooFast`, `idCommandSkipped`, `idCommandCmdKillPrevented`) | **[V]** |
| Kernel drivers run IDBUS "power handshakes" before allowing charging | `IOAccessoryPowerHandshake = Yes` in the live dump; the 2400 mA budget our charger policy consumes is the product of exactly that negotiation ([ds-charging-policy.md](ds-charging-policy.md)) | **[V]** |
| HOSTID is a 16-bit value carried in the 0x74 request data | the article's own captured request is `74 00 02 …` (data `00 02`) and our dump has `IOAccessoryManagerSystemID = 2` | values **[V]**, the identification of the two **[I]** |

Also present in the dump but not yet explained: `IOAccessoryID = 91` (with
spbdimka's list, 0x91 is the AuthenticationInfoResponse handler — plausibly
"the last completed ID-bus exchange", i.e. the Mac cable authenticated
successfully; or an ID-bus request type) **[I]**, `IOAccessoryMagicPullDown
= 3` **[I]**, `IOAccessoryOrientation = 1` (matches the 0x0c bit-6 orientation
from [tristar-recon.md](tristar-recon.md)), and a non-matching
`AppleIDAMInterface` (ID Authentication Module, the MFi coprocessor
interface) **[V]**.

Naming note: an earlier draft of [ds-usb-host.md](ds-usb-host.md) described
the AID bus as "slow I²C" (from Apple code comments). The article's captures
show the wire protocol is **1-Wire-like, not I²C-framed** (§3). Keep both
readings in mind until `AppleTriStarIC`'s I²C-side command semantics are
reconstructed in Ghidra; do not build anything that assumes I²C framing on the
ID pins. **[I]**

## 3. IDBUS physical layer

**[E]** throughout this section, from the author's logic-analyzer captures
(sample rate ≥ 2 MHz recommended for reproduction):

- Two ID lines: **port pins 4 and 8** (ID0 and ID1, one per physical side of
  the mirrored connector — see §4). Tristar polls each ID line in rotation,
  one after another, and eventually gives up if nothing answers.
- Line idles **high** (pull-up; the ~1.1 ms idle interval at the start of a
  poll charges the HiFive's internal capacitor, which powers its logic).
  **[I]** for "idles high", from the scope plot description; the rest is [E].
- A **word** = a `fall–rise–fall` sequence. Two stages:
  - **Meaningful stage** — the interval whose duration encodes the symbol;
  - **Recovery stage** — the following interval, needed by the receiver (and
    by the sender to prepare the next word).
- Word timings (µs):

| Word | Meaningful min/typ/max | Recovery min/typ | Meaning |
|---|---|---|---|
| BREAK | 12 / 14 / 16 | 2.5 / 4.5 | starts a request; also ends the data stage |
| WAKE | 22 / 24 / 27 | ~1100 (?) | slave-initiated re-enumeration (§8) |
| ZERO | 6 / 7 / 8 | 3 | bit 0 |
| ONE | 1 / 1.7 / 2.5 | 8.5 | bit 1 |
| ZERO with STOP | 6 / 7 / 8 | 16 | last bit of a byte is 0 |
| ONE with STOP | 1 / 1.7 / 2.5 | 21 | last bit of a byte is 1 |

- Order of bits within a byte is **not stated** by the source. 1-Wire sends
  LSB first and IDBUS is "very similar to 1-Wire" — treat LSB-first as the
  working hypothesis and confirm from a capture before relying on it. **[I]**
- Timing rules: the slave must wait ≥ 2.5 µs after the closing BREAK before
  replying; Tristar waits **~2.2 ms** for a reply, and if none comes, moves
  on to poll the other ID line.

## 4. Connector pinout

Transcribed **[E]** (OCR of the article's port-pinout image; pins numbered
1–8, mirrored between receptacle and plug):

| Pin | Signal |
|---|---|
| 1 | GND |
| 2 | L0p (lane 0 positive) |
| 3 | L0n (lane 0 negative) |
| 4 | **ID0** (identification/control 0 — mux name ACC1) |
| 5 | PWR (VBUS) |
| 6 | L1n (lane 1 negative) |
| 7 | L1p (lane 1 positive) |
| 8 | **ID1** (identification/control 1 — mux name ACC2) |

The mapping "ID0 ⇒ ACC1 / ID1 ⇒ ACC2" follows from the ACCx tables in §5
(the table for "ID found on ID0" keeps ACC1 in IDBUS mode at reset, and vice
versa) **[I]**. The two lane pairs L0/L1 are the mux's DP1/DN1 and DP2/DN2
pairs; which one is "1" vs "2" depends on orientation **[I]**.

## 5. Packet format, GetID, and mux routing

### 5.1 Framing **[E]**

A request is: `BREAK · byte(type) · bytes(data…) · BREAK`, then the slave
replies (type + data + CRC, no leading BREAK documented). The article's
captured example:

```
74 00 02 1f        # GetID request, data 00 02 (HOSTID), CRC8
```

- **Type byte**: even = request, odd = response, response = request + 1
  (0x74 → 0x75). A few requests (e.g. 0x84 SystemInfo) expect no response.
- **Data**: 0–N bytes, can be empty.
- **CRC8**: over type byte + all data bytes, appended last.

### 5.2 CRC8 — direction matters (verified against the source's own example)

The article's prose says "polynomial 0x31, initial value 0xFF". The spbdimka
command list says "calculate the CRC8 … using the generator **0x8C**". These
are the same polynomial written in opposite bit orders — and only the
reflected direction reproduces the article's own captured frame:

| Algorithm | CRC of `74 00 02` |
|---|---|
| MSB-first, poly 0x31, init 0xFF | `0x2f` ✗ |
| **LSB-first, generator 0x8C, init 0xFF** | **`0x1f` ✓** |

**[V]** (recomputed here from the published example; the direction claim is
ours, both inputs are the article's). Reference implementation, compiled and
checked against the test vector (`74 00 02 → 1f`):

```c
/* Lightning IDBUS CRC8: generator 0x8c (reversed 0x31), init 0xff,
 * LSB-first. Test vector: {0x74, 0x00, 0x02} -> 0x1f. */
static u8 idbus_crc8(const u8 *data, size_t len)
{
        u8 crc = 0xff;

        while (len--) {
                int i;
                crc ^= *data++;
                for (i = 0; i < 8; i++)
                        crc = (crc >> 1) ^ ((crc & 1) ? 0x8c : 0);
        }
        return crc;
}
```

### 5.3 GetID: request 0x74 / response 0x75 **[E]**

The most important exchange. The 0x74 request carries a 16-bit **HOSTID**
in its data (the article's capture: `00 02`; see the cross-check table for
the `IOAccessoryManagerSystemID = 2` match). An invalid HOSTID can make a
genuine HiFive refuse to keep working (Kong/Kanzi have a `disableIdCheck`
firmware variable to ignore it).

The 0x75 response data is a **6-byte ID**. Byte 0 layout (from the THS7383
datasheet tables):

| bits 7:6 | bits 5:4 | bits 3:0 |
|---|---|---|
| ACCx[1:0] | Dx[1:0] | DATA[43:40] |

Bytes 1–5 carry the rest of the ID data (the remaining 40 bits) **[I]** for
the exact bit packing. The ACCx/Dx fields tell Tristar how to wire its
internal buses onto the connector pins **after** identification, and which
physical side is which is decided by which ID line answered (ID0 or ID1 —
i.e. orientation).

**ACCx — configuration of the two single-ended accessory-control pins (ACC1 =
the ID0 pin, ACC2 = the ID1 pin) and HOST_RESET:**

| ACCx[1:0] | ACC1 | ACC2 | HOST_RESET |
|---|---|---|---|
| 00 | Hi-Z, stays IDBUS (ID found on ID0) | Hi-Z, stays IDBUS (ID found on ID1) | Hi-Z |
| 01 | UART1_RX | UART1_TX | Hi-Z |
| 10 | JTAG_DIO | JTAG_CLK | Hi-Z |
| 11 | Hi-Z | Hi-Z | HIGH |

(The "stays IDBUS" column picks ACC1 when the ID was found on ID0 and ACC2
when found on ID1; the 01/10/11 rows are identical for both orientations.)

**Dx — configuration of the two lane pairs:**

ID found on **ID0**:

| Dx[1:0] | DP1 | DN1 | DP2 | DN2 |
|---|---|---|---|---|
| 00 | Hi-Z | Hi-Z | Hi-Z | Hi-Z |
| 01 | USB0_DP | USB0_DN | Hi-Z | Hi-Z |
| 10 | USB0_DP | USB0_DN | UART1_TX | UART1_RX |
| 11 | Hi-Z | Hi-Z | Hi-Z | Hi-Z |

ID found on **ID1**:

| Dx[1:0] | DP1 | DN1 | DP2 | DN2 |
|---|---|---|---|---|
| 00 | Hi-Z | Hi-Z | Hi-Z | Hi-Z |
| 01 | Hi-Z | Hi-Z | USB0_DP | USB0_DN |
| 10 | USB0_DP | USB0_DN | UART1_TX | UART1_RX |
| 11 | Hi-Z | Hi-Z | Hi-Z | Hi-Z |

Note Dx = 10 is orientation-independent (both lane pairs are connected, one
to USB and one to UART), while Dx = 01 puts USB on the lane pair adjacent to
the side that answered — that is how a flip-agnostic cable works **[I]**.

**Known accessory IDs** (the source's table header says HOSTID = 1, while its
own example request carries `00 02` — a minor internal inconsistency of the
source; don't rely on either value; routing decoded here with the tables
above, so the decode itself is exact arithmetic on **[E]** data):

| Accessory | 6-byte ID | ACCx | Dx | Decoded routing |
|---|---|---|---|---|
| Lightning-USB cable (article example; **our Mac cable, live [V]**) | `10 0C 00 00 00 00` | 00 | 01 | USB0 on DP1/DN1, other pair idle, ACC pins stay IDBUS |
| DCSD | `20 00 00 00 00 00` | 00 | 10 | USB0 on DP1/DN1 **and** UART1 on DP2/DN2 |
| KongSWD, no Astris | `20 02 00 00 00 00` | 00 | 10 | as DCSD |
| KongSWD, Astris running | `A0 00 00 00 00 00` | 10 | 10 | JTAG on ACC pins, USB + UART on lanes |
| KanziSWD, no Astris | `20 0E 00 00 00 00` | 00 | 10 | as DCSD |
| KanziSWD, Astris running | `A0 0C 00 00 00 00` | 10 | 10 | as Kong+Astris |
| Haywire (HDMI) | `0B F0 00 00 00 00` | 00 | 00 | both lane pairs Hi-Z (HDMI rides its own pins) |
| UART charge | `20 00 10 00 00 00` | 00 | 10 | as DCSD, plus data bits |
| Lightning→3.5 mm / EarPods | `04 F1 00 00 00 00` | 00 | 01 | USB0 on DP1/DN1 — the internal DAC enumerates as USB |

The **camera adapter's ID is not in the source** and must be captured (§10).

## 6. Full IDBUS command list

OCR'd from @spbdimka's table (Apple-internal handler names; request numbers
in parentheses follow from the even/odd parity rule — **[E]**, with OCR
ambiguities, so re-check any number against Ghidra before use):

| Bytes | Command / handler |
|---|---|
| 0x70/0x73 | module state (`IDBCommandState`); 0x70 is also the power-handshake request (§7) |
| (0x74)/0x75 | Get ID / digital ID (`IDBCommandID`) — carries HOSTID out, 6-byte ID back |
| (0x76)/0x77 | interface device info (`IDBCommandInterfaceDeviceInfo`) |
| (0x78)/0x79 | interface module serial number (`IDBCommandInterfaceModuleSerialNumber`) |
| (0x7A)/0x7B | accessory serial number (`IDBCommandAccessorySerialNumber`) |
| 0x7C/0x7D | GetESNResponse/GetESNRequest (labels as printed; breaks parity — verify) |
| 0x7E | ServiceModeRequest |
| 0x80, (0x82)/0x83 | GetAccessoryInfo; AccessoryInfoVersion (request 0x82 inferred) |
| 0x84 | SystemInfo — **no response** (captured example in the article) |
| 0x8C | internal: "calculate the CRC8 … generator 0x8C" |
| (0x90)/0x91 | AuthenticationInfoResponse |
| 0x92/0x93 | GetAuthenticationCertificate / AuthenticationCertificate |
| (…) | Start/CheckAuthenticationResponse, GetAuthenticationSignature handlers (numbers lost to OCR) |
| 0x9B | LastError |
| 0xB1–0xBE | Bulk Data family: Identify (0xB1), Endpoint Info 0xB2/0xB3, Read 0xBB/0xBC, Bulk 0xBD/0xBE, Continue (0xBF?) |
| 0xE0/0xE2/0xE3 | program OTP / OTP Status / OTP Status Response |
| 0xE5 | SelfTest Control Response |

The serial-number exchanges (0x78–0x7B) explain properties our dump already
shows for the attached cable: `IOAccessoryAccessorySerialNumber =
"REDACTED"` **[V]**, likely the product of the 0x7A/0x7B exchange
**[I]**.

## 7. Power handshakes **[E]** (least-reversed part of the source)

A Lightning accessory's HiFive gates the PWR line to **~10–15 mA** until a
0x74 request is issued and processed; that is the article's explanation for
why a bare cable on a charger passes almost nothing. SecureROM/iBoot only
need the 0x74; once a kernel is up, the kernel Tristar driver additionally
runs:

1. two 0x70 requests back to back;
2. on the reply to the second, **current is disabled entirely for ~20 ms**;
3. a third 0x70 request, this time with **0x80 in its data**;
4. only then is charging from that accessory allowed.

Corroboration on our side is indirect but present: `IOAccessoryPowerHandshake
= Yes` in the live dump, and the 2400 mA budget that
[ds-charging-policy.md](ds-charging-policy.md) consumes is the output of this
negotiation **[V]** that the handshake happens on iOS, **[E]** for the exact
sequence. The register-level I²C commands Tristar needs to execute this are
**not** in the source — they live in the AppleTriStar kexts in our kernelcache
(§10, step 3).

## 8. WAKE re-enumeration **[E]**

The accessory can send a **WAKE** word unprompted; Tristar then restarts IDBUS
enumeration (new 0x74/0x75) and re-routes the mux. That is how a Kong/Kanzi
probe switches from DCSD-like (USB+UART) to SWD mode when Astris launches
*after* the probe was already connected. Any accessory that changes its mind
about its pin configuration uses this mechanism — keep it in mind for the
camera adapter, which similarly needs to renegotiate after it has first
enumerated the tablet as a USB device ([ds-usb-host.md](ds-usb-host.md) §3.1:
vendor request 0x51 → 50 ms → `configureUSBMode(3)`). Whether the adapter also
issues a WAKE first is unmeasured **[I]**.

## 9. Tristar I²C side, ESN, provisioning, electrical limits

- **I²C**: 7-bit 0x1a (= 8-bit 0x34 write / 0x35 read in the source) — the
  diags/kernel interface to Tristar; register map mostly known only from
  leaked iBoot source (THS7383/CBTL1610), which we do not have. Our own
  verified register knowledge is in
  [tristar-recon.md](tristar-recon.md): 0x0f revision, 0x0c VBUS/orientation,
  0x0d events (not fully understood), plus `setVBUSSink` register 0x05 bit 0
  from [ds-usb-host.md](ds-usb-host.md) §3.5. **[V]**
- **ESN**: a blob in Tristar's EEPROM (CBTL1610A2+), retrievable over IDBUS
  with the Serial Number Reader cable/Kanzi; sent to `ttrs.apple.com` to look
  up a dead device's serial number. Apple-internal tooling: `tristar --esn`,
  `tristar --prov_stat`, `tristar -p` (pin configuration) in diags (iOS 7+),
  `accctl` on NonUI/InternalUI builds (reportedly runnable on jailbroken
  retail). **[E]**
- **Provisioning (writing an ESN) is out of scope and forbidden on this
  device.** It is an EEPROM write to factory-programmed state, squarely in
  the AGENTS.md spirit of "never write anything a restore can't bring back";
  even the source author failed to reverse the write algorithm. Reading ESN
  has no use for Linux bring-up; skip both, and never contact
  `ttrs.apple.com`.
- **Electrical** **[E]**: Tristar runs on a 1.8 V supply and its IDBUS lines
  measured ~3.0 V on a scope. If anyone ever builds a hardware sniffing rig
  on pins 4/8 (a passthrough breakout is needed), it must be 3 V-class —
  **not** 5 V-tolerant microcontroller GPIO (Arduino-class) without a level
  shifter.

## 10. What this means for the camera-adapter work

The architecture in one paragraph: **Tristar is the IDBUS master and the mux;
the SoC never speaks IDBUS itself.** Tristar autonomously polls the ID lines,
performs the 0x74 exchange, and routes the lanes. A Linux driver's job is the
I²C register interface — commanding Tristar through the sequences Apple's
kexts use (power handshake, ID retrieval, event handling on 0x0d) and acting
on the results (enable accessory power, flip PHY + `USB_CTL`, register
EHCI/OHCI). The article supplies the *semantics* of what those sequences do;
our Ghidra kernelcache work supplies the *register commands*. They fit
together; neither alone is enough.

Current state: the manual role-switch trial is built but
`hardware_tested=false` ([usb-host.md](usb-host.md)). Its gap list said
"a correct automatic path needs real adapter captures and ID-bus/routing
reconstruction". The reconstruction now exists (this file); the captures do
not. Specifically unknown:

1. **The camera adapter's 6-byte ID** — not in the source's table (§5). Given
   the adapter contains a hub/dual-role controller and negotiates via USB
   vendor request 0x51 after enumerating the tablet as a device, expect an ID
   with Dx routing USB0 onto a lane pair, but that is **[I]** until captured.
2. Whether Tristar's autonomous IDBUS exchange already routes the pins
   correctly with **no SoC-side help** (then the manual trial should
   enumerate), or whether the adapter stalls waiting for the kernel-side
   power-handshake / 0x74 follow-ups before it powers or re-routes.
3. The I²C register commands AppleTriStar/AppleTriStarIC use to command
   GetID, the power handshake, and mux reconfiguration, and the full meaning
   of the 0x0d event register (our current supplier deliberately leaves it
   alone — [tristar-recon.md](tristar-recon.md)).

Ordered next steps (for the orchestrator; none of them is blocked on this
file's [E] claims):

1. **Run the morning trial exactly as written** in
   [usb-host.md](usb-host.md). If a low-power keyboard enumerates, the dumb
   path works and everything below is just the "make it automatic" backlog.
   Third-party Lightning OTG adapters (e.g. the USB-C-female audio/data OTG
   adapters) are the same accessory class as the camera adapter — same trial
   path, same ID-capture step — with one practical difference: a single-port
   OTG adapter cannot charge the iPad while hosting (the USB3 camera
   adapter's Lightning power input can), and a USB-C female side means hubs
   plug in directly. Prefer self-powered hubs for the 100 mA root budget,
   and avoid hubs that backfeed VBUS upstream while the iPad is sourcing.
   Any USB-C plug only changes the physical connector: the port is USB2
   (480 Mb/s, no SuperSpeed, no alt mode) either way.
2. **Capture the adapter's ID under iOS** — cheap, no new hardware, jailbroken
   iPad: attach the Camera Adapter with iOS running, then pull a fresh ioreg
   via `scripts/periph/pull_firmware.py --ioreg` (run by the orchestrator over
   `scripts/ipad-ssh.sh`) and diff against
   `build/firmware/private/IOService.txt`. Do it **twice — once with the
   adapter alone, once with a charger on the adapter's Lightning power
   input**: the USB3 adapter feeds VBUS *into* the iPad (charging path) while
   the data pins run host mode, so its two configurations may expose
   different IDs, power-handshake states and charge-while-hosting behavior
   **[I]**. Extract: `IOAccessoryDigitalID` (the adapter's 6-byte ID),
   `IOAccessoryID`, `IOAccessoryOrientation`,
   `IOAccessoryUSBModeType`/`ConnectTypePublished` transitions,
   `IOAccessoryPowerHandshake`, and the `statistics` ID-bus counters. Decode
   ACCx/Dx with §5's tables. While there, check whether `accctl` exists on
   the device (it is an internal tool; ioreg is the fallback that already
   worked for the Mac cable). With the charger attached, the iPad need not
   source VBUS at all — the accessory side is fed from the adapter's power
   input — which takes our unverified iPad-sourcing question off the
   critical path for this setup; only the data path (Tristar auto-routing)
   remains open.
3. **If the manual host switch does not enumerate** a known-good
   low-power device across all power-gate combinations: Ghidra round on the
   AppleTriStar/AppleTriStarIC kexts in the shared kernelcache analysis to
   decode the I²C register sequences (GetID command, power-handshake 0x70
   sequence, mux control, 0x0d event bits). Use §5–§7 semantics to recognize
   each sequence in the decompilation; rename/comment and save per the shared
   Ghidra workflow.
4. **Optional hardware capture** (only if a logic analyzer and a
   Lightning passthrough breakout show up): pins 4/8, ≥ 2 MHz, §3's word
   table decodes it; compare the adapter's ID-stage exchange against iOS's
   to confirm Tristar needs no SoC-side nudging.
5. **Integration** into `scripts/periph/kernel/apple_tristar_power.c` and the
   role-switch driver: 0x0d event consumption, accessory ID retrieval, power
   handshake, then auto role-switch on the adapter's ID instead of the manual
   sysfs arm. Keep `IOAccessoryDigitalID`-equivalent evidence in the trial
   logs so a future failure can be diffed. Downstream device support beyond
   storage/HID is a config round, not a driver problem: `morning.config`
   covers usb-storage/UAS and HID only, so ethernet (rtl8152, asix,
   cdc_ether), serial (ftdi_sio, cdc_acm) or USB audio (snd-usb-audio) need
   adding when those devices actually get attached through the adapter/hub.

Safety envelope unchanged: read-only Tristar work; never ESN provisioning
(§9); never syscfg/NVRAM/boot firmware.

## Evidence pointers

- External source: <https://nyansatan.github.io/lightning/> (NyanSatan, July
  2020; credits @key2fr, @_L1ngL1ng_, @spbdimka). Images fetched and OCR'd
  locally on 2026-10-26 (tesseract); transcriptions are embedded in §4/§6.
- Our cross-checks: `build/firmware/private/IODeviceTree.txt` (ADT, tristar
  node), `build/firmware/private/IOService.txt` lines ~7660–7790 (AppleTriStar2
  properties, AIDBus transport, IOReport LightningPort channels, statistics),
  `docs/adt/j71t.txt` ~2405–2413, plus [tristar-recon.md](tristar-recon.md)
  and [ds-usb-host.md](ds-usb-host.md).
- CRC direction verification: recomputation of the article's example frame
  `74 00 02 → 1f`; MSB-first 0x31 yields `0x2f`, LSB-first generator 0x8C
  yields `0x1f` (§5.2).
