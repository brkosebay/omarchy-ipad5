# Orientation on J71t without an AOP takeover — path study

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. iPad 5, iPad6,11, S8003 / J71t. **No device access, no kernel edit,
no commit** in this round. Everything below comes from the saved live ADT planes,
the iOS 16.7.10 kernelcache / extracted kexts, and the pinned 20H350 AOP
firmware image.

This document does not re-do
[`aop-sensors.md`](aop-sensors.md),
[`ds-aop-accel.md`](ds-aop-accel.md),
[`ds-aop-takeover.md`](ds-aop-takeover.md),
[`ds-aop-running-image.md`](ds-aop-running-image.md),
[`ds-aop-recovery.md`](ds-aop-recovery.md) or
[`ds-als-hall.md`](ds-als-hall.md). It answers one question — is there a way to
get gravity without taking the AOP away from its current owner — and it adds the
hardware identity and bus topology those documents did not have.

Tags: **[V]** read from an artifact (instruction/decompilation, ADT property,
firmware string or table, saved capture); **[V-dev]** verified on this iPad by a
capture already on disk; **[I]** inference that fits the evidence but is not
measured; **[U]** unresolved (needs a measurement or a further static check).
This is the `ds-aop-running-image.md` tag set; `[V]` is the same thing as `[E]`
in the older AOP documents.

Sources (absolute unless stated):

| Artifact | Path |
| --- | --- |
| live IODeviceTree plane | `build/firmware/private/IODeviceTree.txt` (line numbers below) |
| live IOService plane | `build/firmware/private/IOService.txt` (line numbers below) |
| static ADT, decoded | `docs/adt/j71t.txt` (line numbers below) |
| iOS kernelcache | `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`, SHA-256 `c6b95fcf…1e0a` |
| AOP firmware | `build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin`, 342336 B, SHA-256 `af5d51a3…47c8` (file offsets below) |
| kexts for local decompilation | `build/periph/aop-r1/kexts/{AppleSPU,RTBuddy}.macho` |

---

## 0. Bottom line

1. **There is no "connect as a client" path in this firmware generation.** Every
   message that sets a client session up — HELLO, EPMAP, AFK version, buffer
   request, ring descriptors, the ready announcement — is **device → AP and
   one-shot**; the AP can only answer. A firmware that already handshook with
   the previous AP will not offer them again, and there is no request that makes
   it. **[V]** §2.2–2.4. The foreign-image refusal in our driver is therefore
   *not* what blocks a client connection. Delete that check and the driver would
   still have no endpoint map and no rings — it would simply fail later, with
   mailbox writes already spent.
2. **The accelerometer is not on any bus the AP owns.** It is an InvenSense
   MPU-6x00-family part (iOS reports `model = mpu6900`, `chip_id = 16`) on the
   AOP's **own SPI master at AP PA `0x210100000`** (SoC `0x10100000`), with chip
   select / interrupt on the AOP GPIO block at `0x2100f0000`. No AP device-tree
   node, driver, pinctrl entry, clock or PMGR consumer exists for either block,
   and iOS instantiates only three SPI controllers (spi1–spi3). **[V]/[I]** §1.
3. **The one path that can produce real samples without touching the AOP is to
   read the ring buffer the AOP already owns.** If the firmware kept the accel
   stream running when the AP died (sensor hubs are built to keep sampling with
   no AP), its 16 KiB RX ring is still in DRAM, still being written, and the AP
   can read it with **zero mailbox traffic and zero MMIO writes**. This is
   unproven — it turns on one question, "does `wptr` still advance?" — and it is
   testable read-only. **[I]** §3.3, §4.
4. **If that ring is dead, the answer to the question as asked is no.** The
   remaining options are an AOP takeover (blocked by identity + no proven power
   cycle) or turning the screen by hand, which already ships (`Super+Alt+R`,
   the bar's Rotate button, the iPad Screen menu). **[V]** §3.2.
5. **Side finding, independent of rotation:** if (3) is true, the AOP has been
   writing into pages Linux now allocates — a latent corruption source in every
   Linux boot so far. Finding the ring is worth doing for that reason alone.
   **[I]** §3.3.

**Most promising path and its risk.** Ring reuse (§3.3): no AOP writes, no
firmware load, no identity question, and it would also hand us the real report
bytes to validate scale, subtype and cadence against the assumptions in
`ds-aop-accel.md`. Its risk is that it may find nothing (the firmware may stop
sampling when the AP disappears, or Linux may have recycled the pages), and even
if it works it is a boot-time hack: the ring's PA is chosen by iOS, so it must be
rediscovered (or pinned by an iOS-side tweak) after any iOS boot, and the pages
must be reserved before the allocator takes them.

---

## 1. Q1 — what the sensor is, and which bus it is on

### 1.1 The AP device tree has no accelerometer, only a service node

Everything the AP knows about the IMU is five `AppleSPUHIDInterface` nodes that
hang under `iop-aop-nub`, not under any bus controller
(`build/firmware/private/IODeviceTree.txt`):

| Node | Line | Class | `service_id` | usage page / usage |
| --- | --- | --- | --- | --- |
| `accel` | 921 | `AppleSPUHIDInterface` | `0x10000007` (268435463) | `0xff00` / 3 |
| `gyro` | 940 | `AppleSPUHIDInterface` | `0x10000008` | `0xff00` / 9 |
| `compass` | 959 | `AppleSPUHIDInterface` | `0x10000009` | `0xff00` / 10 |
| `pressure` | 975 | `AppleSPUHIDInterface` | `0x1000000b` | `0x14` / 49 |
| `AOP_spherecontrol` | 991 | `AppleSPUAppInterface` | `0x10000002` | — |
| `mca` | 1005 | `AppleSPUAppInterface` | `0x10000004` | — |

These are *service* nodes created from the firmware's ready reports; they have no
`reg`, no `interrupts`, no `spi-frequency` and no parent bus. **[V]**

The AP's actual buses, complete (`IOService.txt`; `IOKitDiagnostics` counts in
the same file):

| Controller | AP PA | Children on this board | Lines |
| --- | --- | --- | --- |
| `i2c0@A110000` | `0x20a110000` | `pmu@3C`, `lp8559@26`, `tristar@1A` | 1730–1813 |
| `i2c2@A112000` | `0x20a112000` | `als1@39` (CT819) | 1814–1862 |
| `spi1@A084000` | `0x20a084000` | `audio-codec@0` (CS42L81) | 1455–1505 |
| `spi2@A088000` | `0x20a088000` | `mesa@0` (biometric) | 1506–1553 |
| `spi3@A08C000` | `0x20a08c000` | `multi-touch@0` | 1554–1599 |
| `i2s-switch`, `mca0/1` | — | audio | 2290–2371 |

`AppleSamsungSPIController` = **3** instances, `AppleARMIICController` = 1 +
`AppleS5L8940XI2CController` = 2. There is no fourth SPI controller, no sensor
I²C bus, and no bus node anywhere in the live tree whose child is the IMU.
**[V]**

The ADT does know a fourth SPI gate that the AP never uses: PMGR device
`"SPI0"`, `id1 = 74`, `index = 23` (`docs/adt/j71t.txt:1002–1012`), one gate below
`spi1`'s `id1 = 75` (`:2209`). Nothing in the AP tree consumes gate 74. It is a
good fit for the clock of the AOP's own SPI master (§1.2), which the firmware can
gate itself — its device table also maps PMGR (`0x10200000`). **[V] gate, [I] use**

### 1.2 The firmware says which part and which bus

The pinned AOP image contains the drivers and their banner strings
(file offsets into `ipad6faop-20H350.bin`):

| Offset | String | Reading |
| --- | --- | --- |
| `0x39370` | `MPU6700.cpp` | accel/gyro driver source name |
| `0x39350`–`0x39368` | `mpu6800`, `mpu6900`, `mpu6975`, `mpu6x00` | probed model table |
| `0x3939b` | `InvenSense` | vendor |
| `0x3937c` | `%s: %s device ID 0x%x %sfound.` | per-model probe by register ID |
| `0x38f2c`/`0x38f33` | `ak8963`, `AKM8963.cpp` | compass driver (AKM AK8963) |
| `0x39452`/`0x39499`/`0x394af` | `BMP284.cpp`, `Bosch`, `Bosch_BMP284` | pressure driver |
| `0x39759` | `spi0` | the AOP's bus device name |
| `0x38cea` | `External client table full` | firmware-side client table exists |
| `0x39947`/`0x3995d` | `MGMT msg=0x%llx st=%x`, `MGMT st=%x` | RTKit management trace |

**So neither Bosch nor ST: the accelerometer and gyro are InvenSense.** The
"Bosch" strings in this image are the BMP280/284 barometer, which is the
`pressure` node. **[V]**

The same image carries a per-target device table at file `0x47300`–`0x48000`
with AP physical addresses and names. The entry that matters:

```text
047e70  00 00 00 00 00 00 00 00 6c 9c 03 01 00 00 00 00
047e80  00 00 10 10 02 00 00 00 00 10 00 00 00 00 00 00   base 0x10100000, space 2, size 0x1000
047e90  30 7e 04 01 59 97 03 01                           ..., 0x01039759 -> "spi0"
```

and, for orientation, the other blocks the firmware instantiates:

| SoC address | AP PA | Size | Name / use | In the AP DT? |
| --- | --- | --- | --- | --- |
| `0x10100000` | `0x210100000` | `0x1000` | **`spi0`** | **no** |
| `0x100f0000` | `0x2100f0000` | `0x10000` | `aop-gpio` (28 pins, 7 int groups) | yes, `role = "AOP"` |
| `0x1013c000` | `0x21013c000` | `0x1000` | systick (the entry's `0x2710` = 10 000 suggests a 10 ms tick) | no |
| `0x10c00000` | `0x210c00000` | `0x3000` | unnamed AOP block, 3 interrupt IDs | no |
| `0x10ca0000` | `0x210ca0000` | `0x1000` | unnamed | no |
| `0x10cc0000`/`0x10cc1000`/`0x10cc4000`/`0x10cc8000` | `0x210cc…` | `0x1000`/`0x3000`/`0x1000`/`0x1000` | unnamed | no |
| `0x10200000` | `0x210200000` | `0x100000` | PMGR | yes |
| `0x05c08000` | `0x205c08000` | `0x4000` | unnamed | no |

Evidence: the table is at the file offsets above. The `0x100f0000` entry carries
seven interrupt descriptors (`0x10007`…`0x1000d`), matching the `aop-gpio` node's
seven interrupts — an independent confirmation that the table really is a
description of this board's AOP-private peripherals. **[V]** for the table,
**[I]** that `spi0` is the sensor bus: it is the only bus controller the firmware
instantiates, there is no I²C controller in the table, and iOS's own IMU driver
sits behind this firmware. Every alternative AP bus is already accounted for in
§1.1.

### 1.3 Can the AP reach that controller?

Physically the bytes are in the AP's mapped window. `/arm-io` translates
(`docs/adt/j71t.txt:3226`, `compatible = "arm-io,s8000"`, `#address-cells = 2`,
`#size-cells = 2`):

```text
child 0x0000000000000000  -> parent 0x0000000200000000  size 0x0000000100000000
child 0x0000000600000000  -> parent 0x0000000600000000  size 0x0000000200000000
```

so SoC `0x10100000` is AP `0x210100000`, inside the first range. **[V]**

Everything else says the AP must not go there:

* **No DT node.** Neither `0x10100000` nor any `0x10c…` block appears anywhere in
  `docs/adt/j71t.txt` or the live tree (`grep -i 10c00000` → 0 hits). Only
  `aop` (`0x10800000`), `aop-gpio` (`0x100f0000`), `0x10000600` and `0x10000508`
  are described, and the last two only as AOP registers.
* **No consumer.** iOS instantiates nothing on it (§1.1); Linux has no node
  either. It is not a "reserved" device — it is simply not described.
* **No clock/reset/pinctrl ownership.** The `aop` node has empty `clock-gates`,
  `clock-ids`, `power-gates` (`IODeviceTree.txt:888, 897, 903`); PMGR gate
  `SPI0`/74 belongs to nobody in the AP tree. There is no AP pin-function
  property for a sensor SPI anywhere in the ADT (the only `function-spi_cs0`
  entries are spi1/2/3, `j71t.txt:2206, 2240, 2247`).
* **The AOP is actively driving it.** Unlike a passive register block, a live
  SPI master is mid-transaction state: its FIFO, clock divider and CS timing
  belong to the firmware's poll loop. Programming it from the AP is a bus
  takeover with a running master, not a read.

**Conclusion:** the AP can address the register page, and nothing in hardware
stops a `/dev/mem` write — but there is no described, owned, or safe way to use
it, and doing so would fight the AOP for the same part. This is not a path.
**[V] + [I]**

### 1.4 GPIO and interrupt

* The IMU's chip select and interrupt are almost certainly on the **AOP's** GPIO
  block (`aop-gpio@100F0000`, AP `0x2100f0000`, `IODeviceTree.txt:1024–1045`):
  28 pins, `#gpio-int-groups = 7`, `supported-int-groups = <5 6>`, seven
  interrupts `113…119` to the AIC, `no-resume-restore`, `role = "AOP"`. **[V]**
  for the block, **[I]** for "the IMU lines are on it" — it is the only GPIO
  controller on the AOP side and iOS has no other way to wire CS/INT.
* No AP node references its phandle (`44`); it has no children in the live tree.
  Its interrupts exist for the AOP/wake path, not for an AP sensor driver. **[V]**
* Which of the 28 lines is the IMU INT / CS is **not** in any AP artifact — it is
  board data inside the firmware's target table. **[U]**
* The only interrupt the AP gets from this subsystem that carries *data* is the
  mailbox: AIC 133 (outbox / IOP→AP), already documented. **[V]**

### 1.5 The part, as iOS sees it

From `AppleSPUHIDDriver` under `accel` (`IOService.txt:1735–1778`):

| Property | Value | Line |
| --- | --- | --- |
| `model` | `mpu6900` | 1757 |
| `manufacturer` | `InvenSense` | 1759 |
| `chip_id` | 16 | 1774 |
| `sensor_rates` | `10 100 200 1000 ` (Hz) | 1741 |
| `dispatchAccel` / `dispatchGyro` | `Yes` / `Yes` | 1756, 1925 |
| `calibration_state` | 2 | 1737 |
| `part`, `silicon` | `0`, `0` | 1746, 1763 |
| `DebugState._num_events` | 14728 | 1747 |
| HID `VendorID`/`ProductID` | 1452 (`0x5ac`) / 33028 (`0x8104`) | 1697, 1703 |
| `MaxInputReportSize` | 22 | 1685 |
| `ReportDescriptor` | `0600ff0a0300a101150026ff00750895168102c0` | 1691 |
| HID `ReportInterval` | 8000 (µs) | 1705 |

The descriptor decodes as usage page `0xff00`, usage 3, one 22-byte input
report — exactly the payload the dormant Linux consumer already parses.
**[V]**

Companion parts on the same firmware: compass `ak8963` / `AKM`, `chip_id = 72`
(`0x48`, the AK8963 `WHO_AM_I`), `sensor_rates = "100 "` (lines 2086–2088, 2103,
2070); pressure `BMP284` / `Bosch`, `chip_id = 40`, rates `1 2 5 10 25`
(2249–2251, 2266, 2234). Neither is AP-visible either — both are on the same AOP
bus. **[V]**

---

## 2. Q2 — "attach as a client" instead of taking over

### 2.1 What the running firmware actually advertises

The live IOService tree gives the complete endpoint map of the instance that is
running on this iPad right now:

| Endpoint | Child of `RTBuddy` | `AppleSPU@<service>` | Interface published | Line |
| --- | --- | --- | --- | --- |
| `AOPEndpoint1` | 1575 | `@10000001` | `SPUApp` (+ `AppleSPUAppDriver`, `AppleSPUProfileDriver`) | 1579, 1599 |
| **`AOPEndpoint2`** | **1636** | **`@10000007`** | **`accel`** → `AppleSPUHIDDevice` → `IOHIDInterface` → `AppleSPUHIDDriver` | **1640, 1660** |
| `AOPEndpoint3` | 1802 | `@10000008` | `gyro` | 1806, 1826 |
| `AOPEndpoint4` | 1968 | `@10000009` | `compass` | 1972 |
| `AOPEndpoint5` | 2132 | `@1000000b` | `pressure` | 2136 |
| `AOPEndpoint6` | 2294 | `@10000004` | `mca` | 2298 |
| `AOPEndpoint9` | 2499 | `@10000002` | `AOP_spherecontrol` | 2503 |
| `AOPEndpoint13/14/17/19` | — | `@1000000c/0d/0e/10` | no interface published | — |

Also `AOPEndpoint7/8/10/11/12/18` exist as `RTBuddyEndpointService` objects with
no `AppleSPU` child. **[V]**

Consequences:

* The accelerometer is behind **endpoint 2**, service **`0x10000007`**, and a
  **per-interface channel** carried in the ready report (§6 of
  `ds-aop-accel.md`). The Linux driver's binding rule (name `accel`, id
  `0x10000007`) is correct. **[V]**
* The endpoint number is assigned by the firmware through EPMAP
  (`base*32 + bit`), so it is the *firmware's* choice. For one fixed firmware
  image it is almost certainly stable — but we have exactly one observation and
  no second boot to compare, so "2" is **[I]**, not a constant to hard-code.
* `slaveAlignment = 64`, `"rx queue size" = 16384`, `"tx queue size" = 16384` on
  every `AppleSPU` node (`IOService.txt:1591–1595`), i.e. the ring buffer is
  16 KiB per direction. **[V]**

### 2.2 Who initiates what

Management layer (`ds-aop-takeover.md` §2.3, re-checked here):

| Message | Dir | When |
| --- | --- | --- |
| HELLO `0x0010…` | device → AP | once, at firmware boot |
| HELLO_REPLY `0x00200000000c000c` | AP → device | in answer |
| EPMAP `0x0080…` | device → AP | once, after HELLO |
| EPMAP reply | AP → device | in answer |
| STARTEP `0x0050 \| ep<<32 \| 1/2` | AP → device | after EPMAP, gated on state ≥ 4 |
| PING / PING_ACK, AP power, IOP power | both | periodic |

Per-endpoint AFK layer (`0xfffffff005ba88e0`, decompiled this round):

| Message | Dir | Meaning |
| --- | --- | --- |
| `0x80` + version 8 | **device → AP** | endpoint initialised; only message accepted before the version is known |
| `0xa0` | AP → device | version ack |
| `0x89` (tag, size) | **device → AP** | "give me a buffer" |
| `0xa1` (PA) | AP → device | here is the buffer |
| `0x8a`/`0x8b`/`0x8c` | **device → AP** | TX / RX / historical ring descriptors inside that buffer |
| `0xa3` | AP → device | start |
| `0x86` | device → AP | start ack |
| `0xa2` | AP → device | TX doorbell (no pointer) |
| `0x85` | device → AP | RX notification |
| `0xc0` name32+service id | **device → AP** | service ready (the accel announcement) |
| `0xa4`/`0xc2` → `0x87`/`0xc3` | AP → device → AP | shutdown / hibernate and ack |

Three details proved from the code this round:

* The switch gates on the version: `if ((int)param_1[0x11] != 0 ||
  (word & 0xff000000000000) == 0x80000000000000)` — i.e. **nothing but `0x80` is
  processed until the device has sent `0x80`**, and `0x80` itself is rejected
  (`"Incompatible protocol version"` panic) unless the low 48 bits are exactly 8.
  `0xfffffff005ba88e0`. **[V]**
* STARTEP is built by `0xfffffff005b717a4` / thunk `0xfffffff005b675b4` as
  `0x0050 | (ep & 0xffff) << 32 | (flag ? 2 : 1)` and **returns without sending
  while `rtbuddy->[0x140] < 4`** — the state that only exists after a boot
  handshake. **[V]**
* The AFK start (`0xfffffff005ba9754`) sends `0xa3` and then blocks on
  `AppleSPU+0x25a`; the stop (`0xfffffff005ba9808`) sends `0xa4`/`0xc2`, blocks
  on `+0x258`, and then only **drains stale messages** (`0xfffffff005ba84d0`) —
  it does *not* free the rings or clear the version. So a stop/start cycle
  reuses the same buffers and never re-negotiates. **[V]**

### 2.3 The minimal "client" handshake, step by step

| # | Needed for a client | Who must send it | Available on the running instance? |
| --- | --- | --- | --- |
| 1 | endpoint map (which number is accel) | device (`0x0080`) | **no** — sent once at boot |
| 2 | AFK version (`0x80`/v8) | device | **no** — sent once per endpoint init |
| 3 | buffer (`0x89`) | device | **no** |
| 4 | ring descriptors (`0x8a`/`0x8b`) | device | **no** |
| 5 | start ack (`0x86`) | device, in answer to `0xa3` | only into the **existing** rings |
| 6 | service announcement (`0xc0`) | device, on the RX ring | **no** — already delivered into a dead buffer |
| 7 | configuration (subtype 4) | AP | needs a TX ring from (4) |
| 8 | reports (`0xc2`) | device, on the RX ring | only into the **existing** ring |

Steps 1–4 and 6 are device-initiated and one-shot; the AP has no message that
asks for them again. **That is the whole answer: a client attach is not a
protocol feature of RTKit-2062 / AFK v8 on this board.** **[V]**

**What "read-only" can mean here, and why it does not help.** There are only
three things an AP can read from this subsystem without a session:

* the wrapper status registers (`+0x44`, `+0xb88`, `+0x4008`, `+0x4020`,
  `+0x4040`) — they say "running / empty / no fault", never a sample;
* the firmware region at `0x210e00000` — it is not the report path, and
  `ds-aop-running-image.md` shows it no longer holds an identifiable image;
* the mailbox FIFO at `0x4038` — but **reading it consumes**, and it is empty on
  this instance, so a passive read returns nothing and permanently discards
  whatever it might have contained.

A passive client is therefore impossible even in principle: the device's data
path is DMA into a ring buffer, and the only thing that names that ring is the
`0x89`/`0x8a`/`0x8b` exchange we cannot trigger. §3.3 is the way around that
constraint — find the ring instead of asking for it.

Apple reached the same conclusion from the other side: `AppleA7IOP::start` on a
core it did not start returns `kIOReturnBusy`, and `RTBuddy::powerStateTransition`
**panics** on it (`ds-aop-takeover.md` §2.1–2.2). The only "attach" iOS knows is
hibernation restore, where the AP-side endpoint objects *and their ring
pointers* come back from the hibernation image. We have no such image. **[V]**

### 2.4 Why the foreign-image refusal is not the blocker

Phase A refuses because the region at `0x210e00000` no longer matches the pinned
image (`ds-aop-running-image.md` §6, `ds-aop-runtime-layout.md`). That check
answers "may I treat this memory and this firmware as mine?" — it is a **load /
write** gate.

**If a client path existed, the region check would indeed be the wrong gate.** A
client never reads or writes the firmware region; what it needs to trust is the
*protocol*, and that is negotiated in band (management `min ≤ 12 ≤ max`, AFK
version 8). So the objection "our attach refused on a foreign image, maybe a
client connection does not need that check" is correct as far as it goes — it
just does not buy anything, because the client path itself is missing.

What a client connection *would* still need:

* the endpoint map (step 1 above), which we cannot get;
* buffers (steps 3–4), which we cannot request;
* confidence that the far-side ring pointers are not pointing into memory Linux
  has since reused.

So the identity problem is real but secondary. Even a perfectly identified
running image gives us nothing to attach to. Conversely, if the protocol *did*
have a re-handshake, a protocol-version check (the `0x80`/v8 and management
`min ≤ 12 ≤ max` negotiations) would be sufficient identification — the SHA-256
of the region would not be needed. **Recommendation: keep the refusal as is.** It
guards the load path, which is still the only path that can *own* the AOP. That
path remains blocked on the reservation/remap/power-cycle work in
`ds-aop-recovery.md` §5.

### 2.5 Levers that could force a re-initialisation — and why none is a client path

| Lever | What it would take | Why it is not the answer |
| --- | --- | --- |
| Re-send STARTEP to endpoint 2 | one type-5 word, 8 bytes | the reference implementation will not send it below RTBuddy state 4 (which only EPMAP produces); a driver of ours could bypass that, but the firmware's behaviour on a second STARTEP is **[U]** — if it re-initialises it must re-issue `0x80`/`0x89` (unproven), and if it treats the word as an error it may panic |
| AFK shutdown (`0xa4`) then start (`0xa3`) | two words | stop does not free rings or reset the version (§2.2), so start resumes into the **old** buffers — useless to a new AP, and it is a write into someone else's session |
| Let the watchdog reset the firmware | nothing (just stop pinging) | that *is* a takeover, with the worst possible timing: unknown reset vector, unknown region contents, no way to load a verified image afterwards |
| Real AON power cycle | hardware / battery | the only clean reset, and the driver still refuses to bring domains up (`ds-aop-recovery.md` §5) |

None of these produces a *client*; all of them are takeovers wearing a smaller
message. **[V]/[I]**

---

## 3. Q3 — fallbacks

### 3.1 AP-visible sensors cannot infer orientation

| Sensor | What it measures | Why it cannot give gravity |
| --- | --- | --- |
| CT819 ALS, i²c2 `0x20a112000`, addr `0x39` | illuminance, 0–5000 lux | one scalar, no direction; already working in Linux (`ds-als-hall.md`) |
| D2365 hall, PMU GPIO lines 0 and 10 | magnet present/absent, 2 bits | 3–4 discrete cover states, unrelated to how the iPad is held |
| PMU ADC / battery / temperature | scalar | — |
| Touch (spi3) | contact coordinates | needs a user; gives no gravity vector |

There is no second accelerometer, no AP-visible IMU, and no non-AOP motion
source on this board. **[V]**

### 3.2 Manual rotation (works today, zero risk)

Already shipped and independent of any sensor:

```sh
ipad-rotate landscape          # or portrait / portrait-flipped / landscape-flipped
ipad-rotate                    # print the current orientation
ipad-rotate lock on|off|toggle # only meaningful while ipad-autorotate runs
```

Bindings: `Super+Alt+R` (rotate by hand), the bar's Rotate button, the iPad
Screen menu; a gesture can be assigned in `~/.config/linux-a9/gestures.conf`
(`docs/omarchy/tablet-usage.md` §Rotation). `ipad-autorotate` stays
`inactive (condition no)` until an IIO accelerometer exists, and its
`--mock`/`--mock-iio` modes let the policy be exercised without hardware.
**[V]**

This is the honest fallback: auto-rotate is a convenience, and the device is
fully usable without it.

### 3.3 The one real no-takeover data path: reuse the AOP's ring

**Hypothesis.** Sensor hubs are built to keep sampling while the AP is asleep;
the AOP batches reports into the AP-allocated ring and wakes the AP when it has
data. When the AP died (panic, DFU, kexec) the firmware was never told; it still
owns endpoint 2, its 16 KiB RX ring, and its write pointer. If it kept the
stream enabled, it is writing 22-byte `0xc2` accel reports into that ring *right
now*, and the ring is ordinary DRAM the AP can read. **[I]**

Supporting evidence:

* The ring lives in AP DRAM, not in the AOP region (§4.1 of `ds-aop-accel.md`:
  the AP allocates it on `0x89` and replies with a 48-bit PA). **[V]**
* The AOP keeps running across AP resets — measured on both a warm kexec and a
  true cold boot (`cpu_running=0x00020001`, `idle=0x79`, empty FIFOs). **[V-dev]**
* The firmware has a client/AP state machine with `ap en` / `ap dis` / `lockup` /
  `recvr` labels (`0x390de`–`0x390f2` in the image), i.e. it does model the AP's
  enable state — which cuts both ways: it may also *stop* on `ap dis`. **[V]
  (strings), [U] (behaviour)**
* The `aop` node lists `AOP.APWatchdog` and `AOP.OutboxNotEmpty` as AP wake
  sources (`j71t.txt:1724`), i.e. the AOP is expected to act when the AP stops
  responding. **[V]**

Counter-evidence, stated plainly: no unexplained memory corruption has been
reported in the Linux runs so far, which is weak evidence that nothing is
writing into reused pages. And a hub that sees its AP disappear may well park
the sensors. **This is a 50/50 hypothesis, and it is cheap to settle.**

**Why it matters even if rotation is not the goal.** If the hypothesis is true,
the AOP has been DMA-writing into pages the Linux allocator owns since the first
boot of this port. That is a correctness bug in the platform, not a feature.

**What it would give us.** The real report bytes: subtype, length, 16.16 g
scale, axis signs, cadence — the exact items U4–U6/U8 in `aop-sensors.md` are
waiting for — with zero mailbox traffic.

**What it cannot give us.** A stable driver: the ring PA is chosen by iOS, so it
changes whenever iOS reallocates; the stream only exists if iOS left it enabled;
and none of it survives an AON power cycle.

---

## 4. Experiments (orchestrator only; nothing below was run)

Order matters: E1 is read-only and settles the hypothesis; E2 only exists if E1
passes.

### E1 — find the ring and watch `wptr` (read-only, no AOP traffic)

The ring header is a strong, cheap signature (`ds-aop-accel.md` §4.3):

```text
+0x00  u32  bufsize = ring size - 0xC0      16 KiB ring -> 0x3F40
+0x04  u32  marker  = 0x00070006
+0x40  u32  rptr    < bufsize                (written by the AP consumer)
+0x80  u32  wptr    < bufsize                (written by the AOP producer)
+0xC0  ...  entries, magic "AOP " / "IOP "
```

`"rx queue size" = 16384` and `slaveAlignment = 64` are in the live dump
(`IOService.txt:1591–1595`), so the expected header is `3f 40 00 00 06 00 07 00`
at a 64-byte-aligned physical address. **[V]**

Implementation sketch (a new, default-off, read-only debugfs file in the existing
`apple-a9-aop` observer — no new MMIO, no mailbox):

1. Walk the kernel's `iomem_resource` tree, **`System RAM` ranges only** (never a
   `reserved`/MMIO range: reading the wrong physical page is how the round-2
   collector got `EFAULT`, and some MMIO blocks have read side effects). For each
   range, read through the linear map (`__va(phys)`) in bounded chunks.
2. Match `u32[0] == size - 0xC0 && u32[1] == 0x00070006` with `rptr`, `wptr` <
   `bufsize`, at 64-byte alignment.
3. For each hit: dump `+0x00..0x100`, sleep 1 s, read `wptr` again, and report
   `advanced=yes/no`, plus a bounded parse of one entry (`magic`, length,
   subheader version/category/subtype).
4. Stop after N hits or the end of RAM; no writes, no MMIO, no clock/power
   change. Reading RAM cannot wedge the AOP.

Pass: a hit whose `wptr` advances and whose entry parses as subtype `0xc2` with
plausible 16.16 g values. Fail: no hit, or every hit static.

Cost: one boot, one kernel patch, no risk to the AOP. This should be the **next
thing done to this problem**, before any further attach work.

If the scan finds multiple rings (tx/rx/historical, and other endpoints), the
accel RX ring is distinguishable by the `0xc2` payloads.

### E2 — make the ring usable (only if E1 passes)

1. Reserve the found PA range at boot. The cleanest route is a
   `/reserved-memory` node carried in the m1n1 payload DT (the same way the AOP
   payload already ships a board-specific DT), not a late `memblock_reserve`
   after the page is in use.
2. Re-run E1 on the next boot and require the same signature at the same PA
   before trusting it — the AOP may reallocate or stop.
3. Only then a small read-only IIO driver: parse the ring, expose
   `in_accel_{x,y,z}_raw` + scale + mount matrix, `-ENODATA` when `wptr` stops.
   No TX, no mailbox, no AOP ownership — the AOP keeps its session and its
   calibration.
4. Re-derive the PA after any iOS boot, or pin it from the iOS side (below).

### E2' — the owner-mediated variant (if E2's PA is too unstable)

Because iOS is jailbroken, an iOS-side tweak could allocate the accel ring at a
fixed PA (or simply report the PA it got), keep the stream enabled at 10 Hz, and
then hand the PA to Linux through the same "boot after iOS" path we already use
(DFU → palera1n → pongoOS → m1n1 does **not** reset the AON island, so the
session survives). Linux then reserves that PA and reads it. This is the
protocol-legal client handoff — the *owner* prepares the session — but it needs
iOS-side code and a boot-flow change, so it is strictly worse than E2 unless the
PA turns out to move on every boot.

### E3 — do not run yet

STARTEP replay (§2.5). One word, but its failure mode is a firmware panic with no
proven recovery. It becomes worth considering only if E1 fails **and** someone
has first established a way to bring the AOP back (the recovery design in
`ds-aop-recovery.md` §5, which is itself unproven).

---

## 5. What would change the answer

| Observation | Consequence |
| --- | --- |
| E1 finds an advancing ring with `0xc2` payloads | orientation is available today without touching the AOP; proceed to E2, and treat the AOP's write into Linux RAM as a bug to fix |
| E1 finds a static ring | the stream was stopped/never enabled; ring reuse is dead, and so is every no-takeover path |
| A second boot shows the accel on a different endpoint number | endpoint numbers are dynamic; the ready-report name, not the number, must be the binding key (already the design) |
| A later boot (iOS or Linux) shows the firmware sending HELLO or EPMAP again | the firmware does re-handshake after losing its AP, which reopens the client question — capture the transcript |
| Someone proves a safe AON-only power cycle | the takeover path opens and this document becomes a footnote |

---

## 6. The three questions, answered

| Question | Answer | Tag |
| --- | --- | --- |
| Which part? | InvenSense MPU-6x00 family — iOS `model=mpu6900`, `manufacturer=InvenSense`, `chip_id=16`; firmware models `mpu6700/6800/6900/6975/mpu6x00`. Bosch is only the BMP284 barometer; there is no ST part. | [V] |
| Which bus? | The AOP's own SPI master at SoC `0x10100000` / AP PA `0x210100000` (`"spi0"` in the firmware's device table); compass AK8963 and pressure BMP284 on the same AOP side. | [V] table, [I] association |
| GPIO / IRQ? | AOP GPIO block `0x2100f0000` (28 pins, 7 int groups to AIC 113–119), `role=AOP`; exact IMU line is board data inside the firmware. No AP GPIO or IRQ carries sensor data; the AP's only data IRQ is the mailbox (AIC 133). | [V]/[U] |
| Can the AP reach the controller? | The address is inside the `arm-io` window, so the bytes are addressable — but there is no DT node, driver, clock, reset or pinmux, iOS never instantiates it, and the AOP is actively driving it. Not a usable path. | [V] |
| Does the running firmware expose a client endpoint? | It exposes endpoints 1–19, accel on **2** (`0x10000007`), but the session setup messages are device-initiated and one-shot. No client attach exists. | [V] |
| Is the foreign-image refusal the blocker? | No. It gates the load path; a client path would fail later, at "no EPMAP / no `0x80` / no rings". Keep it. | [V] |
| Any AP-visible sensor for orientation? | No. ALS is a scalar, the hall is 2 bits, touch needs a user. | [V] |
| Fallback? | Manual rotation, already shipped (`Super+Alt+R`, bar button, iPad Screen menu). | [V] |
| Path to real data without a takeover? | Only ring reuse (§3.3), and it is unproven until E1 runs. | [I] |

---

## Appendix A — addresses and offsets cited

Kernelcache unslid VAs unless stated. Firmware offsets are into
`ipad6faop-20H350.bin`; firmware VAs in the image are `0x01000000 + offset`.

| Address / offset | What |
| --- | --- |
| `fffffff005ba88e0` | AFK endpoint switch: version gate, `0x80`→`0xa0`, `0x89`→`0xa1`, ring descriptors, `0x86`/`0x87` wakeups |
| `fffffff005ba8bb4` | GET_BUF handler (`0x89`/legacy `0x81`) → allocate + `0xa1 \| pa` |
| `fffffff005ba9754` | AFK start: `0xa3`, wait on `AppleSPU+0x25a` |
| `fffffff005ba9808` | AFK stop: `0xa4`/`0xc2`, wait on `+0x258`, then drain |
| `fffffff005ba84d0` | post-stop drain of stale messages (rings are *not* freed) |
| `fffffff005b717a4` / thunk `fffffff005b675b4` | STARTEP builder: `0x0050 \| ep<<32 \| 1/2`, gated on `rtbuddy+0x140 >= 4` |
| `fffffff005ba7984` | `AppleSPU::start` — installs handlers, ends with endpoint `+0xb0` (STARTEP) |
| `fffffff005b7196c` | `handleHello` (`0x00200000000c000c`) |
| `fffffff005b71afc` | `handleEndpointMap` |
| `fffffff005b71018` | management dispatch |
| `0x01000000`+`0x39759` | firmware string `"spi0"` |
| `0x01000000`+`0x47e80` | firmware target-table entry: base `0x10100000`, space 2, size `0x1000`, name `spi0` |
| `0x01000000`+`0x47b00` | `aop-gpio` entry: base `0x100f0000`, size `0x10000`, 7 interrupt IDs |
| `0x01000000`+`0x39370` | `MPU6700.cpp`, and the `mpu6xxx`/`InvenSense` strings |
| `0x01000000`+`0x390de` | firmware client-state labels `ap en`, `ap dis`, `lockup`, `recvr` |
| live ADT `IODeviceTree.txt:883–1045` | `aop@10800000`, `iop-aop-nub`, `accel`…`mca`, `aop-gpio@100F0000` |
| live IOService `IOService.txt:1522–1541` | `RTBuddy`, `FirmwareUUID/Version`, `RTKitVersion` |
| live IOService `IOService.txt:1575–1799` | `AOPEndpoint1/2`, `AppleSPU@1000000x`, `accel`, HID device and driver |
| `docs/adt/j71t.txt:1002` | PMGR device `SPI0`, `id1 = 74` (unused by the AP) |
| `docs/adt/j71t.txt:3226` | `/arm-io` ranges: SoC→AP translation |
| `docs/adt/j71t.txt:1724` | AOP wake sources, incl. `AOP.APWatchdog` |

## Appendix B — reproduction

```sh
# part identity and endpoint map (no device needed)
grep -n '"model"\|"manufacturer"\|"chip_id"\|"sensor_rates"' \
  build/firmware/private/IOService.txt | sed -n '1,40p'
sed -n '1630,1680p' build/firmware/private/IOService.txt    # AOPEndpoint2 -> accel

# firmware strings and the device table
strings -a -t x build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin |
  grep -iE 'mpu6|InvenSense|ak8963|BMP284|spi0' 
python3 - <<'PY'
b=open('build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin','rb').read()
for off in range(0x47e70,0x47ea0,16):
    print(f'{off:06x}', ' '.join(f'{x:02x}' for x in b[off:off+16]))
PY

# the AFK switch and STARTEP gate (offline, no shared server)
~/.local/share/uv/tools/ghidra-mcp/bin/python scripts/periph/ghidra-local.py \
  --binary build/periph/aop-r1/kexts/AppleSPU.macho \
  --output build/periph/aop-r9/afk-switch --function 0xfffffff005ba88e0
~/.local/share/uv/tools/ghidra-mcp/bin/python scripts/periph/ghidra-local.py \
  --binary build/periph/aop-r1/kexts/RTBuddy.macho \
  --output build/periph/aop-r9/startep --function 0xfffffff005b675b4
```

The shared Ghidra project no longer contains `fffffff005ba88e0`
(`FUNCTION_NOT_FOUND`); the local extractor above produced the listing used here
under the ignored `build/periph/aop-r9/`.
