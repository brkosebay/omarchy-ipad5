# AOP SPI0 channel 3 — warm-reload round 3

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. Scope: offline analysis of the pinned iOS 16.7.10 j71t AOP
firmware and the matching shared iOS kernelcache. This follows
`ds-aop-reload-crash-and-wake.md`, `ds-aop-resume-round2.md`, and the
2026-10-26 16:27 entry in `aop-stage3.md`.

Evidence labels: **[V]** verified directly in code or a device log, **[I]**
inference from verified facts, **[U]** unresolved.

## Result

1. SPI0 channel **3 is the MPU6900 asynchronous FIFO/data channel**. The same
   MPU object uses channel **4** for synchronous register commands, AOP GPIO
   pin **14** (`0xe`) for its external trigger/interrupt route, and numeric
   routing/chip-select selector **4**. The detected part is not just a family
   guess: cold device logs say `mpu6900 device ID 0x10 found`. **[V]**
2. The crash-producing interrupt is SPI0's channel-fault/class-0 aggregate,
   AOP interrupt ID **`0x00010012`**. Its status register is SPI0 `+0x8d0` and
   channel `n` occupies bit `2*n`; channel 3 is therefore **bit 6**. **[V]**
3. The minimum production-safe bridge is to perform the firmware's own
   channel teardown on channel 3 after the controller reset: clear channel run
   bit 0 and wait for idle bit 1, mask both per-channel IRQ classes, clear the
   channel descriptor, then apply the fault handler's acknowledge action.
   This is five MMIO writes plus a bounded poll; every value comes from a
   firmware path. **[V]** for the writes, **[I]** that they are sufficient
   until the device experiment.
4. Do **not** merely remove the pre-RUN check. Fresh firmware enables IRQ
   `0x10012` while all channel-table slots are still NULL, then resets SPI0,
   and allocates the MPU's channel 3 later. A pending bit at RUN can therefore
   execute the same NULL `table[3]+0x4c` load at `0x0102f252`. **[V]**
5. SPI0 reset's provider transition 3 is unrelated. It writes `0x0000000c`
   to the **systick** block at AOP VA `0x1013c000` / AP PA `0x21013c000`; it
   does not touch SPI0, AOP GPIO, or the MPU. **[V]**

## Pinned evidence

The repeatable analysis is `scripts/periph/aop/re-spi0-ch3-round3.py`. It pins:

- main SHA256
  `af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8`;
- extension SHA256
  `2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22`.

It exports the focused decompilations to the ignored directory
`build/periph/aop-stage3/re/spi0-ch3-round3/`, annotates the long-lived private
main-firmware project, and creates an annotated private combined
main+extension project for the board-specific MPU code.

The shared kernelcache confirms the AP-side boundary:

- `AppleSPU_stopEndpoint`, `0xfffffff005ba9808`, only sends AFK `0xa4` (or
  hibernate `0xc2`), waits for queue-stopped, and drains stale ring messages;
- `AppleSPUHIDDriver_setSensorProperties`, `0xfffffff005b99ef0`, treats a zero
  report interval separately and sends property keys `0x16=0`, `0x5c=0`, and
  `0=0` in that order through the command-gate wrapper now named
  `AppleSPUInterface_setPropertyCommandGated` at `0xfffffff005bad638`;
- the kernelcache has no MPU6900/MPU6xxx hardware driver or AOP-private SPI0
  channel knowledge. The AP talks to an AOP service, not to the IMU bus.

## 1. What channel 3 is

### 1.1 Board wiring recovered from the extension

The j71t MPU constructor at `0x01072ed0` calls the common SPI-client
constructor at `0x01005c38` with:

```text
r0 = MPU object 0x01064f40 + 0xac
r1 = 4       synchronous command channel
r2 = 3       asynchronous FIFO/data channel
r3 = 0x0e    AOP GPIO pin
stack[0] = 4 routing/chip-select selector
```

The numeric role of the final `4` is verified through the common constructor:
it is stored in each SPI handle at `+0x04`; channel configuration copies it to
configuration `+0x24`; SPI master event `0x14` then writes:

```text
SPI0 + 0x004 = 1 << (4 + 16) = 0x00100000
channel + 0x04 = 1 << 4      = 0x00000010
```

Calling it the routing/chip-select selector is **[I]**; the value and its
register use are **[V]**. GPIO `0x0e` is not a guess: after enabling async
trigger mode, `0x01005f70` calls the AOP GPIO object's configuration method
with `(pin=0x0e, command=0x807, argument=0)`. **[V]**

The object is the MPU6xxx class:

- its vtable is at `0x010368a4` and its board start method is
  `0x01072cac`;
- the adjacent driver literals identify `MPU6700.cpp`, `mpu6800`, `mpu6900`,
  `mpu6975`, `mpu6x00`, and `InvenSense`;
- device syslog in
  `docs/periph/logs/20261025-1805-aop-stage3-enablefix-full-dmesg-excerpt.txt`
  reports `mpu6900 device ID 0x10 found`.

Thus the mappings are:

| Resource | MPU6900 use | Evidence |
| --- | --- | --- |
| SPI0 channel 4 | synchronous register commands | constructor `r1=4`; common sync-transfer handle at `+0x20/+0x28` |
| SPI0 channel 3 | asynchronous FIFO/data transfer | constructor `r2=3`; async handle at `+0x00/+0x08`; start path `0x0100ee18` |
| AOP GPIO 14 | external trigger/interrupt pin configuration | constructor `r3=0xe`; `0x01005f70` sends GPIO command `0x807` |
| routing/CS selector 4 | SPI route/chip select | constructor stack argument; SPI0 `+0x4` bit 20 and channel `+0x4` bit 4 |

Endpoint `0x21` is only the AP-facing AFK/EPIC service endpoint. It is not an
SPI channel, GPIO number, or chip select.

### 1.2 Controller interrupt identity

The SPI0 object at `0x01047e78` contains these three interrupt IDs:

| Object field | AOP IRQ | Handler | Status |
| --- | --- | --- | --- |
| `+0x44` | `0x00010014` | `0x0102f0c8`, controller transfer | controller/global |
| `+0x3c` | `0x00010010` | `0x0102f1b4`, channel class 1 | `SPI0+0x8d4`, bit `2*n` |
| `+0x40` | `0x00010012` | `0x0102f20a`, channel fault/class 0 | `SPI0+0x8d0`, bit `2*n` |

The warm value `SPI0+0x8d0 = 0x40` therefore means exactly:

```text
bit 6 = bit (2 * 3) = channel 3 fault/class-0 pending
```

It does not identify an AOP-GPIO interrupt group directly. The likely physical
chain is MPU GPIO14 level/DRDY -> already-armed SPI0 async channel 3 -> SPI0
fault/class-0 aggregate IRQ `0x10012`. **[V]** for the endpoints of that chain,
**[I]** for the precise electrical cause of the reassertion.

## 2. Channel registers and fresh initialization

For channel `n`, the register block begins at:

```text
SPI0 + 0x100 + 0x80*n
```

For channel 3 at AP base `0x210100000`:

| Name in this note | AP address / SPI0 offset | Known use |
| --- | --- | --- |
| `C3` / logged `ch3` | `0x210100280` / `+0x280` | enable, idle, async mode, callback IRQ masks/status |
| `C3_ROUTE` | `0x210100284` / `+0x284` | route mask; fresh MPU value `0x10` |
| `C3_RATE` | `0x210100288` / `+0x288` | timing/rate field |
| `C3C` / logged `ch3c` | `0x21010028c` / `+0x28c` | async-mode bit 3; recovery/ack bit 5 |
| `C3_DESC` | `0x210100298` / `+0x298` | channel `+0x18` descriptor, zeroed on teardown |

### 2.1 Proven `C3` bits

| Bit | Firmware operation | Meaning supported by code |
| --- | --- | --- |
| 0 | `setChannelEnabled(true)` writes `(C3 & ~0x411) | 1`; false writes `C3 & ~0x411` | channel run/enable |
| 1 | disable polls until this bit is 1 | channel idle acknowledgement, read-only in these paths |
| 2 | `setAsyncTriggerMode(true)` writes `(C3 & ~0x414) | 4` | async/external-trigger mode arm |
| 6 | callback type 0 writes `(C3 & ~0x450) | 0x40`; NULL callback omits `0x40` | `+0x8d0` class-0/fault callback IRQ enable |
| 8 | callback type 1 writes `(C3 & ~0x510) | 0x100`; NULL callback omits `0x100` | `+0x8d4` class-1 callback IRQ enable |
| 4, 10 | cleared by the enable/mask operations above; event `0x14` initially writes bit 10 | coupled control/status bits; exact names **[U]** |
| 16 | present after the reset, not assigned by the recovered methods | **[U]** |

For `C3C`:

- bit 3 is explicitly set/cleared by `setAsyncTriggerMode(..., argument&1)`;
- bit 5 is ORed by the class-0/fault handler before it examines the software
  channel object, and fresh event `0x14` also ORs it;
- observed bits 0 and 21 have not been named. Do not rewrite them wholesale.

The successful reset produced:

```text
C3  = 0x00010447 = bits 16,10,6,2,1,0
C3C = 0x00200009 = bits 21,3,0
```

So the controller came out of reset idle (`bit1=1`) but still enabled
(`bit0=1`), in async-trigger mode (`bit2=1`), with the class-0/fault IRQ
enabled (`bit6=1`). This is enough for a level source to repopulate
`SPI0+0x8d0 bit6` while the AOP CPU remains halted. **[V]/[I]**

### 2.2 What fresh MPU initialization does

`MPU6xxx_configureSPIChannels_j71t` at `0x01072d5e` starts with the zeroed
runtime template at `0x01053900` (that address is BSS, not the trailing file
metadata), then writes:

```text
config + 0x54 = 0x007a1200   (8,000,000)
config + 0x58 = 1
config + 0x5c = 1
config + 0x60 = 8
config + 0x64 = 1 (byte)
```

It dispatches lifecycle event `0x14` first for channel 4, then channel 3.
For each channel, `AOPSPIMaster_handleEvent`:

1. allocates and zeroes a `0x70`-byte channel object if absent;
2. **stores its pointer into `channel_table[channel]`;**
3. sets the software initialized flag;
4. writes `channel+0x00 = 0x400`;
5. writes the route registers (`SPI0+0x4=0x00100000`,
   `channel+0x4=0x10` for selector 4);
6. programs timing/descriptor fields and ORs `channel+0xc` with `0x20`.

After both channels exist, `0x01005f70` puts channel 3 in async-trigger mode:

```text
C3C bit 3 = 1
C3 = (C3 & ~0x414) | 0x4
AOP GPIO pin 14 config command = 0x807
```

The later async-read path `0x0100ee18` registers the class-0 callback (which
sets `C3 bit6`) and starts the channel (which sets `C3 bit0`). **[V]**

## 3. What can keep channel 3 quiet

### 3.1 Exact channel teardown — recommended

The common client stop at `0x01005f18` calls
`AOPSPIMaster_teardownChannel` (`0x0102e4f8`). Its hardware operations are:

```text
/* C3 = SPI0+0x280, C3_DESC = SPI0+0x298 */
write32(C3, read32(C3) & ~0x00000411);  /* clear bits 0,4,10 */
poll until (read32(C3) & 0x00000002) != 0;  /* bounded on Linux */

write32(C3, read32(C3) & ~0x00000450);  /* mask class 0: bits 4,6,10 */
write32(C3, read32(C3) & ~0x00000510);  /* mask class 1: bits 4,8,10 */
write32(C3_DESC, 0x00000000);           /* channel +0x18 */
```

The fault handler at `0x0102f20a` acknowledges the already-latched condition
before its unsafe software dereference with:

```text
write32(C3,  read32(C3)  & ~0x00000450);
write32(C3C, read32(C3C) |  0x00000020);
```

Combining teardown with that final `C3C` action is the smallest sequence that
both stops stale channel activity and applies the firmware's own pending-fault
recovery. Starting from the observed post-reset values, expected readbacks are:

```text
C3:      0x00010447 -> 0x00010046 -> 0x00010006
C3C:     0x00200009 -> 0x00200029
C3_DESC: old value  -> 0x00000000
```

All accesses must remain inside the existing SPI0 PMGR guard, with the AOP CPU
halted and AP notifications masked. Every poll must be bounded. Preserve all
unmentioned bits with read-modify-write. **[V]** for register behavior.

Why not only clear bit 6? The handler prefix `C3 &= ~0x450; C3C |= 0x20` is
probably sufficient to suppress IRQ `0x10012`, but it leaves channel run bit 0
and its transfer descriptor live while the new image is copied. The full
teardown costs three additional writes and prevents a stale transfer engine from
touching old SRAM buffers. **[I]**

### 3.2 No proved FIFO flush

No recovered method establishes a FIFO flush register. `C3_DESC=0` is a
descriptor clear, not evidence of a FIFO drain. Do not invent a write to a
neighbor register. If the exact teardown still leaves `+0x8d0 bit6` asserted,
the next investigation should log `+0x280..+0x2a0` and recover the remaining
channel methods before adding writes. **[V]/[U]**

### 3.3 Sensor stop was already attempted

The shared iOS kernelcache gives a fully specified AP protocol stop: set
batch (`0x16`), raw (`0x5c`), then report (`0`) intervals to zero, waiting for
each reply. The Linux quiesce already does exactly this in
`a9_aop_set_rate(aop, 0, 0, 0)`.

The retained shutdown transcript proves success:

```text
AOP-PREV QUIESCE step=rates-zero status=0 accel=0
```

Here `accel=0` is the valid interface index, not “no accelerometer”; the code
calls the three property transactions when `accel >= 0`. Yet the pre-reset
channel was still `0x18011445` and the reset produced the armed
`0x00010447`. Therefore repeating rate-zero or AFK stop is not the missing
operation. **[V]**

The MPU driver's internal disable routine at `0x0100e8b6` is nevertheless
useful corroboration. Its disable branch writes:

```text
MPU register 0x6c = cached_0x6c | 0x3f  /* PWR_MGMT_2: axes standby */
MPU register 0x6b = cached_0x6b | 0x40  /* PWR_MGMT_1: sleep */
then AOPSPIClient_stopAsyncTransfer(channel 3)
```

There is no proof that the zero-rate property reaches that branch when other
firmware clients retain the IMU, and issuing those MPU writes directly from
the AP would require a separately proved SPI command framing/CS transaction.
Do not add raw sensor writes in this round. **[V]/[U]**

### 3.4 Provider transition 3 cannot help

The pointer at `AOPSPIMaster+0x18` is the systick object at `0x01047e30`, base
`0x1013c000`. Its event-3 branch at `0x0102fc04` is exactly:

```text
lock(object + 0x40)
*(u32 *)0x1013c000 = 0x0000000c
unlock(object + 0x40)
```

The AP alias is `0x21013c000`. Event 5 updates systick `+0x3c`; neither path
touches SPI0, GPIO, or MPU state. Replaying provider transition 3 would improve
fidelity to `AOPSPIMaster_reset`, but cannot suppress channel 3. **[V]**

## 4. Can fresh firmware handle a pending channel 3?

### 4.1 If the table is populated first: yes

For event `0x14`, allocation ordering is safe: the new zeroed channel object
is stored into `channel_table[3]` before any channel register write. If IRQ
`0x10012` is held off until after that store, the handler can process a
pending channel 3 without a NULL dereference. It first masks/acknowledges the
hardware, then reads the valid object's flags and optional callback. A newly
zeroed object has neither the active flag nor callback, so the remainder is a
safe no-op. **[V]**

### 4.2 Actual boot ordering: no

Fresh boot does not provide that ordering. SPI lifecycle event 1:

1. allocates and zeroes the 16-entry pointer table;
2. maps SPI0;
3. registers and enables IRQs `0x10014`, `0x10010`, and **`0x10012`**;
4. dispatches event 5, which calls the controller reset;
5. returns long before the board MPU start configures channels 4 and 3 with
   event `0x14`.

Thus `0x10012` is enabled while `channel_table[3] == NULL`. The handler at
`0x0102f20a` does not test the pointer:

```text
status = SPI0[0x8d0]
if (status & (1 << (2*3))) {
        C3 &= ~0x450
        C3C |= 0x20
        ch = channel_table[3]       /* NULL during early boot */
        if (ch->flags_at_0x4c & 1) /* fault at 0x0102f252, DFAR 0x4c */
                ...
}
```

The handler does perform the correct hardware clear before the bad load, but
that is too late for this boot. A pending condition can preempt immediately
after IRQ enable and before event 5's reset; the round-1 crash is precisely
this window. **[V]**

Consequences:

- the existing pre-RUN refusal is correct;
- dropping it without first masking/tearing down channel 3 reintroduces the
  known crash;
- after the proposed AP teardown, fresh firmware can safely re-establish the
  channel because event `0x14` installs `table[3]` before it later re-enables
  channel-3 callback bit 6 and run bit 0.

## 5. Next device experiment: one exact channel-3 teardown

This experiment changes only the halted warm-reload path. Keep the controller
reset and every existing refusal/guard.

### Procedure

After `0x0102f2f4` replay reports `8d0 bit6 = 0`, before copying the new image:

1. Snapshot and log `SPI0 + {0x280,0x284,0x288,0x28c,0x298,0x8d0,0x8d4}`.
2. Execute, in this order:

   ```text
   C3 = read32(+0x280)
   write32(+0x280, C3 & ~0x411)
   poll +0x280 bit1 == 1, same bounded timeout discipline as reset
   write32(+0x280, read32(+0x280) & ~0x450)
   write32(+0x280, read32(+0x280) & ~0x510)
   write32(+0x298, 0)
   write32(+0x28c, read32(+0x28c) | 0x20)
   ```

3. Read back every written word and require:

   ```text
   (+0x280 & (BIT(0)|BIT(6)|BIT(8))) == 0
   (+0x280 & BIT(1)) != 0
   +0x298 == 0
   (+0x28c & BIT(5)) != 0
   (+0x8d0 & BIT(6)) == 0
   ```

4. Copy the image as today, then delay/recheck for at least **150 ms** (longer
   than the observed approximately 105 ms load window). Do not RUN unless
   `+0x8d0 bit6` remains zero.
5. On pass, release RUN once and require normal HELLO/EPMAP/AFK startup and
   MPU6900 accel reports. Do not retry on a failed boot.

### Pass

- idle poll succeeds;
- exact readbacks match;
- `+0x8d0 bit6` stays zero through the 150 ms pre-RUN window;
- the new firmware reaches HELLO and later reports
  `mpu6900 device ID 0x10 found` / produces accel samples.

This proves the retained cause is the old channel-3 engine/IRQ configuration,
and the teardown is sufficient for production reload.

### Fail

Any of these is a fail and must retain the current no-RUN behavior:

- idle bit 1 does not assert within the bounded poll;
- a write does not read back;
- `+0x8d0 bit6` reasserts while bit 6 is masked;
- the fresh firmware crashes before table[3] allocation;
- HELLO succeeds but MPU probe/streaming fails.

If bit 6 reasserts despite the exact masks, preserve that trace and stop. The
next round should recover the meaning of the remaining channel neighbor/FIFO
registers; it should not escalate to guessed FIFO or raw MPU writes.

## Ghidra changes

The private firmware projects now name the SPI client/channel methods, systick
provider event handler, MPU streaming/init methods, and the j71t MPU
constructor/channel setup. Comments mark the early-IRQ race, the exact teardown
masks, the fault acknowledge prefix, the channel-3 wiring, and the systick-only
provider transition.

The shared kernelcache now names
`AppleSPUInterface_setPropertyCommandGated` and comments the separation between
rate-zero sensor properties and AFK endpoint stop. The shared project was
persisted with `scripts/ghidra-mcp.sh save` (`SAVED ok, changed now=False`).
