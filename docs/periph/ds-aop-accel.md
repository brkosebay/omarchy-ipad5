# J71t AOP accelerometer — driver study (round 3)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25 correction: **`+0xb88` is not verified CPU status.** It mirrors
inbox `+0x4008` in every saved boot today, including FIFO occupancy. Earlier
running/halted conclusions from its bit0 are withdrawn; execution remains
unknown. The current forced-cold path accepts RUN `+0x44` bit4 clear after
10 ms, with auto-boot/outbox disabled and fault-free mailboxes, under the
owner's accepted crash risk. See [aop-force-cold.md](aop-force-cold.md).
Historical captures and earlier path descriptions below retain their provenance.


2026-10-23. iPad 5, iPad6,11, S8003 / J71t. **No device access was performed in
this round**; every statement below comes from the saved ADT, the private
IODeviceTree plane, the shared iOS 16.7.10 kernelcache, the extracted kexts, or
the existing Linux sources. No commit was made, no kernel branch was touched.

> **Errata (round 3 implementation, 2026-10-23):** a follow-up static pass
> corrected the ring header ownership (the AOP writes it), the ring entry layout
> (channel at entry+8, whole EPIC packet at +0x10, wrap via a tail header
> copy, conditional doorbell), the ext segment placement (DRAM, `EC0p`), the
> tag-table writes, BSS handling, application endpoint numbering (`>= 0x20`)
> and the boot-complete message (mgmt type 7 = 0x20). See
> [`aop-sensors.md` → Round 3](aop-sensors.md#round-3--active-driver); the
> table there supersedes §4.3, §4.4, §5.4, §7.1 (+4 channel) and §10.2–10.3.

This round closes the queue/ownership and endpoint/message-sequence unknowns
that round 2 left before an active driver could attach and read the
accelerometer. It does **not** claim that the AOP is running under our boot
path; that measurement is still the first gate.

Evidence is tagged as in [`ds-als-hall.md`](ds-als-hall.md):

- **[E]** read directly from an artifact (ADT, IODeviceTree dump, kext
  instructions/decompilation, firmware file, existing source).
- **[I]** inference that fits the evidence but is not proven on this board.
- **[U]** unresolved; needs a measurement or a further static check.

All kernel addresses are **unslid virtual addresses** in:

```text
build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12
SHA256 c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a
Ghidra: kernelcache.release.iPad6,11_12; AARCH64:LE:64:AppleSilicon
```

The shared Ghidra server stopped answering during this round. It was **not
restarted**, per `AGENTS.md`. All new decompilation was done offline with
`scripts/periph/ghidra-local.py` on the same extracted kexts already used in
round 1 (`build/periph/aop-r1/kexts/`). The addresses agree with the shared
project's (e.g. `AppleSPU_routeEPICPacket` is `fffffff005ba83ec` in both).
New retained artifacts are under `build/periph/aop-r3/`.

---

## 0. Result in one table

| Question from round 2 | Answer | Tag |
| --- | --- | --- |
| Who allocates the shared buffer? | The **AOP firmware asks** with AFK type `0x89`; the **AP allocates** an IOKit buffer memory descriptor and replies with its physical address. The ADT AOP region is *not* the queue storage. | [E] |
| Where do queue addresses come from? | Firmware sends tagged descriptors `0x8a`/`0x8b`/`0x8c`; offset and size are in 64-byte units **inside the buffer the AP just allocated**. The AP creates a ring view at `buffer + offset`. | [E] |
| Queue alignment | `slaveAlignment = 0x40` when the DT has no `aop-memory-alignment` (j71t has none). The A9 IOP's default alignment method literally returns `0x40`. | [E] |
| Ring header layout | 3 blocks of 0x40: `bufsize` + marker `0x00070006` at +0x00, `rptr` at +0x40, `wptr` at +0x80, payload at +0xC0. The AP initializes it. | [E] |
| Ring entry layout | `magic` (`"AOP "` is the only constant the host writer builds; accept `"IOP "` too) + `u32` entry size + first 8 bytes of the EPIC header, then the rest of the EPIC packet at +0x10. Advance 0x40-aligned, wrap at `bufsize`. | [E] fields; [I] size/length relation and magic direction |
| Endpoint numbers | Assigned by firmware via management EPMAP: `endpoint = base*32 + bit`, `base` in bits 37:32, bitmap in bits 31:0, last flag bit 51. The host does not choose the accel endpoint. | [E] |
| Accel identity | Ready-report subtype `0xc0`, 32-byte name `"accel"`, service id `0x10000007`; the per-interface **channel** selects the stream. | [E] name/id; [E] channel field; [I] wire offset +4 |
| Attach to a running AOP | No replay/takeover message exists. A warm attach is only defensible if the boot handshake (HELLO / EPMAP / AFK INIT) is still pending in the outbox FIFO. If the FIFO is empty and the CPU is running, **refuse**. | [E]+[I] |
| Was the AOP started by iBoot? | The artifact says **no**: `IsLoadedByiBoot = false`, `IsFUDFirmware = true`, no `pre-loaded`/`running` nub properties. The kernel's AppleSPUFirmwareService loads it through FUD. | [E] |
| Rate | Property `0` (report interval) in **microseconds**; `0` stops. Batch key `0x16`, raw key `0x5c` are sent first. | [I] (strong) |
| Sample | Subtype `0xc2`; payload `seq16@0`, `status32@2`, `x/y/z s32@6/10/14`, `temp32@18`; signed 16.16 g. | [E] layout, [I] units |
| ARot | iOS sends the ADT `accel-orientation` blob to firmware property `0x18`, then uses the axes unchanged. Do **not** apply it again on the AP; expose a measured residual mount matrix. | [E] behavior, [U] blob semantics |
| Close | Rate 0/0/0, then AFK shutdown `0xa4<<48`; the device answers `0x87<<48` (normal) or `0xc3<<48` (hibernate variant `0xc2`). | [E] words, [I] mapping |

---

## 1. Evidence identity and method

Kexts used (from `build/periph/kexts/prelink.json`):

| Bundle | Executable header | `__TEXT_EXEC` range used here |
| --- | --- | --- |
| `com.apple.driver.IOSlaveProcessor` | `fffffff005303f80` | `fffffff005b1f000`–`fffffff005b20144` |
| `com.apple.driver.AppleA7IOP` | `fffffff00530e300` | `fffffff005b4b000`–`fffffff005b55e58` |
| `com.apple.driver.RTBuddy` | `fffffff005310840` | `fffffff005b56000`–`fffffff005b82214` |
| `com.apple.driver.AppleSPU` | `fffffff005319e00` | `fffffff005b83000`–`fffffff005bb589c` |

Method:

1. Re-read the round-1 function list and the saved decompilations in
   `build/periph/aop-r1/`.
2. Extracted the four kexts' `__TEXT_EXEC` instruction ranges and scanned the
   raw words offline for `movz`/`movk`/`adrp+add` immediates (message opcodes,
   string references, vtable slots).
3. Decompiled or instruction-listed the exact entry points with
   `scripts/periph/ghidra-local.py` into `build/periph/aop-r3/{spu,rtbuddy,
   iop,slave}-local*/`. Every claim below cites the entry address, not a
   decompiler-generated name.
4. Cross-checked the message words against the a9/aop protocol header already
   in `patches/aop/round2/` and against the mainline RTKit source in
   `build/boot/round4-source/drivers/soc/apple/rtkit.c`. Where the old j71t
   firmware differs, the difference is called out.

Reproduction of the important pieces:

```sh
# offline instruction listings / decompilation (no shared server needed)
~/.local/share/uv/tools/ghidra-mcp/bin/python scripts/periph/ghidra-local.py \
  --binary build/periph/aop-r1/kexts/AppleSPU.macho \
  --output build/periph/aop-r3/spu-local \
  --function 0xfffffff005ba88e0

python3 -B scripts/periph/aop-recon.py words 0xfffffff006d5e7f8 0x90
```

The old SPU protocol is **not** the modern AFK/EPIC revision used by Asahi on
Apple Silicon. The type numbers overlap, but ring framing, direction labels and
the shutdown words differ; see §5.7 and §5.8.

### 1.1 Function index

Names are descriptive; the address is the exact unslid entry used in this
report. Every one was read from the j71t kernelcache, not inferred from a
symbol table.

| Function | Address | Role |
| --- | --- | --- |
| `RTBuddy::sendMessage` | `fffffff005b66aa8` | packs endpoint bits 63:56, validates endpoint/state |
| `RTBuddy::allocateBuffer` wrapper | `fffffff005b671b4` | gated call to the allocator action |
| `RTBuddy::allocateBuffer` action | `fffffff005b67264` | IOKit buffer descriptor, 0x40 alignment, PA check |
| `RTBuddy` management dispatch | `fffffff005b71018` | type 1/4/8/0xb RX switch |
| `RTBuddyManagement_handleHello` | `fffffff005b7196c` | replies `0x00200000000c000c` |
| `RTBuddyManagement_handleEndpointMap` | `fffffff005b71afc` | base/bitmap/last, creates endpoints |
| `RTBuddy::createEndpoint` | `fffffff005b661e4` | system endpoints vs `RTBuddyEndpointService` |
| `RTBuddyEndpointService` create | `fffffff005b6aa30` | service + endpoint pair |
| `RTBuddyEndpoint` init | `fffffff005b6a848` | links service, rtbuddy, endpoint number |
| `RTBuddyEndpoint` send | `fffffff005b6a230` | `send(rtbuddy, [this+0x88], msg)` |
| `RTBuddy` ping | `fffffff005b71f1c` | type 3, seq/uptime word |
| `RTBuddy` AP power | `fffffff005b716d0` | type 0xa, 0/1 |
| `RTBuddy` watchdog timer | `fffffff005b71268` | ping cadence/timeout |
| `AppleSPU::start` | `fffffff005ba7984` | alignment, endpoint enable, callbacks |
| AFK endpoint switch | `fffffff005ba88e0` | 0x80/0x85/0x86/0x87/0x89–0x8f/0xa2/0xc3 |
| GET_BUF path | `fffffff005ba8bb4` | allocate + reply `0xa1 \| pa` |
| Queue constructor | `fffffff005baaf2c` | ring view at buffer+offset |
| Ring init | `fffffff005bab1c8` | bufsize/marker/rptr/wptr |
| Ring write | `fffffff005bab540` | magic + size + split EPIC header |
| TX doorbell | `fffffff005bab130` | bare `0xa2<<48` |
| AFK start | `fffffff005ba9754` | `0xa3<<48`, waits on `+0x25a` |
| AFK stop | `fffffff005ba9808` | `0xa4`/`0xc2`, waits on `+0x258` |
| `AppleSPU_routeEPICPacket` | `fffffff005ba83ec` | outer version, ready report, channel |
| `AppleSPU_handleReadyReport` | `fffffff005ba8554` | name32/service id/channel |
| Interface constructor | `fffffff005baefb4` | `+0x90` id, `+0x94` channel |
| `AppleSPUInterface_walkSubpackets` | `fffffff005bacde4` | advances 24 + length |
| `AppleSPUInterface_dispatchSubpacket` | `fffffff005bacf58` | category/subtype dispatch |
| `AppleSPUHIDInterface_deliverReport` | `fffffff005bad4d8` | payload to HID |
| `AppleSPUInterface_setPropertyGated` | `fffffff005bad898` | subtype-4 notify |
| Property packet builder | `fffffff005badefc` | key/value, +4 length, zero prefix |
| Subheader builder | `fffffff005bae168` | 24-byte subheader |
| `makeSubheaderV2` | `fffffff005bae308` | exact-length variant |
| TX enqueue | `fffffff005bae344` | outer header + subheader + doorbell |
| Outer header builder | `fffffff005baf0fc` | version 2, seq, channel/reserved, timestamp |
| TX ring action | `fffffff005ba8398` | writes via `+0x150`, channel from `+0x94` |
| Rate setter | `fffffff005b99ef0` | keys 0x16/0x5c/0 |
| Motion report dispatch | `fffffff005b9eb5c` | subtype 0xc2 |
| Factory calibration | `fffffff005b9d9e4` | table walk, property 0x18 |
| Report descriptor | `fffffff005b8d550` | subtype 1 |
| `AppleA7IOP` start | `fffffff005b4b738` | power/mailbox/CPU release |
| `AppleA7IOP` remap | `fffffff005b4e72c` | wrapper +0x18..+0x38 |
| `AppleA7IOP` mailbox bulk | `fffffff005b4bda8` | 8 bytes per FIFO word |
| `AppleA7IOP` default alignment | `fffffff005b4bb44` | returns `0x40` |
| `IOSlaveProcessor` alignment forwarder | `fffffff005b1f8ec` | calls IOSlaveCPU `+0x578` |

---

## 2. What the transport is (fixed from round 1, restated for the recipe)

`AppleA7IOPV2` → `RTBuddy` → `AppleSPU`. It is a **64-bit IOP mailbox**; there
is no 96-bit ASC doorbell and no MSI.

| Item | Value | Tag |
| --- | --- | --- |
| Wrapper | `0x210800000`, size `0x1c000` | [E] |
| CPU control | `+0x44` | [E] |
| Raw vendor predicate (mirrors inbox; execution unknown) | `+0xb88` bit 0 | [E] read; semantics unverified |
| Inbox control | `+0x4008` (bit16 full, bit17 empty, bits18/19 fault) | [E] |
| AP→IOP FIFO | `+0x4010`, 64-bit, one `str` per message | [E] |
| Outbox control | `+0x4020` (bit17 empty, bit0 enable, bits18/19 fault) | [E] |
| IOP→AP FIFO | `+0x4038`, 64-bit, one `ldr` per message; **reading consumes** | [E] |
| IRQ | AIC 133 = outbox/non-empty (RX), AIC 130 = inbox space (TX) | [E] |
| AOP target region | `0x210e00000`, live size `0x9b000` (static ADT says `0xa0000`) | [E] |
| Region property | `region-base` is already absolute | [E] |

Word format:

```text
bits 63:56   endpoint
bits 55:0    payload; management type is bits 55:52, AFK type is bits 55:48
```

`RTBuddy::sendMessage` (`fffffff005b66aa8`) proves the encoding and rejects a
nonzero high byte that disagrees with the target endpoint:

```text
if (payload_high_byte != 0 && payload_high_byte != endpoint) return EINVAL;
wire = (payload & 0x00ffffffffffffff) | (endpoint << 56);
```

It also requires `endpoint < endpoint_count` and an internal state `>= 4`.
Therefore the driver must not send STARTEP or AFK traffic before the endpoint
map has been processed.

The AOP does not need a DART/IOMMU entry: the live IODeviceTree has no
`dart-aop`/`iommu-parent` under `aop@10800000`, and the allocator returns
the descriptor's physical-segment address (+0x80) directly (§4.1). Queue
buffers are ordinary AP physical DRAM addresses.

---

## 3. Attach: what "iBoot already started it" actually means

The round-2 premise ("attach to an AOP that iBoot already started") is **not
supported by the artifacts**:

- `Firmware/AOP/aopfw-ipad6faop.im4p` has `IsFUDFirmware = true`,
  `IsLoadedByiBoot = false`, `IsLoadedByiBootStage1 = false`.
- The saved live `iop-aop-nub` has no `pre-loaded`, `running` or
  `no-firmware-service` property.
- `AppleSPUFirmwareService_start` (`fffffff005b87340`) implements the FUD
  request/response path and explicitly distinguishes pre-loaded (state 3) from
  normal (state 2).
- `AppleA7IOP` has a "CPU already running" branch, but that is a hardware
  state check, not evidence that iBoot loaded the image.

Consequences for the driver:

| Observed state at probe | Action |
| --- | --- |
| Domains off/transitioning | Report and skip; never force a domain on. |
| `cpu_running = 0` | Cold path: reserve the region, parse/load the firmware, program remap/patchbay, start CPU. **Blocked on U2** (region size + remap/patchbay contract). |
| `cpu_running = 1`, outbox FIFO holds a boot handshake | First-attach path: drain a **bounded transcript** (reading consumes), require a type-1 HELLO for endpoint 0, then follow §5.6. This is only safe if the firmware has not been initialized by a previous AP driver. |
| `cpu_running = 1`, outbox FIFO empty | **Refuse.** The firmware is owned by a previous AP (iOS or a previous Linux boot); its ring pointers refer to memory we do not own. There is no RTKit "takeover" or "reset queues" message. |
| Mailbox fault bits 18/19 set | Stop; do not acknowledge or clear. |

The cold capture supplied in `build/periph/aop-live/r2-cold-capture.txt` does
**not** settle this: it looks for
`/sys/kernel/debug/apple_a9_aop/` (underscores), while the driver and
`scripts/periph/aop/capture.sh` use `/sys/kernel/debug/apple-a9-aop/`
(hyphens). Both files are from the same command, so the capture cannot
distinguish "probe failed" from "wrong path". Re-run the pass with the
documented path before drawing any conclusion about U1.

---

## 4. Shared memory: who allocates it, and what the layout is

### 4.1 The allocation path (AOP asks, AP allocates)

The endpoint message switch is `fffffff005ba88e0`. Case `0x89` is the buffer
request:

```text
word = (0x89 << 48) | (size_blocks << 16) | tag
size = size_blocks * 64
```

`fffffff005ba8bb4`–`fffffff005ba8c54` then:

1. Rejects the request if the buffer table already has 16 entries.
2. Rounds the size up to the cache line and calls
   `(*(provider_vtable + 0x558))(provider, 0, size)`
   (`provider` is the RTBuddy device object at `AppleSPU + 0xa0`).
3. Reads two descriptor properties with `+0x78` and `+0x80`; the second is the
   48-bit address sent to the AOP, i.e. its physical-segment address. (A kernel
   virtual address would not fit the 48-bit reply mask.)
4. Stores `{tag, descriptor, va, pa, size}` in the 0x28-byte table entry.
5. Replies:

```text
word = (0xa1 << 48) | (pa & 0x0000ffffffffffff)
```

The provider's `+0x558` is `fffffff005b671b4`, a command-gate wrapper whose
action is `fffffff005b67264`:

- `fffffff005b6d000(flags, size, offset, alignment, backing, ..., 0x40)`
  creates the memory descriptor with **0x40-byte alignment**;
- `(*(descriptor + 0x80))()` is checked for a valid physical segment;
- `(*(descriptor + 0x88))(descriptor, 0, 0)` prepares it.

So: **the queues are host-allocated DRAM, not the ADT AOP region.** The AOP
region is the firmware/target-memory aperture; the shared rings live wherever
the AP's allocator puts them, and the AOP addresses them by AP physical
address. There is no IOMMU translation step in the path.

For Linux, `dma_alloc_coherent()` (or an equivalent non-cacheable allocation)
with 0x40 alignment is the matching primitive. Keep the allocation until the
endpoint has been shut down; the firmware may write to it asynchronously. Make
sure the device's DMA ops are **direct** (no IOMMU domain): the value placed in
the `0xa1` reply must be the AP physical address, not an IOVA. If the platform
device ends up behind an IOMMU, allocate from a reserved CMA/gen_pool region
and translate with `phys_to_dma()`/`dma_to_phys()` explicitly, or the AOP will
read the wrong memory.

### 4.2 Queue descriptors

The firmware follows with `0x8a`, `0x8b`, `0x8c`:

```text
word = (type << 48) | (offset_blocks << 32) | (size_blocks << 16) | tag
offset = offset_blocks * 64
size   = size_blocks   * 64
```

- `0x8a` = **AP→IOP ring** (host writes it). iOS creates it at `AppleSPU+0x150`.
- `0x8b` = **IOP→AP ring** (host reads it). iOS creates it at `AppleSPU+0xd0`
  and schedules it from the `0x85` notification.
- `0x8c` = historical ring (`AppleSPU+0x1d0`); create it if offered, do not
  enable it for basic rotation.

The iOS log strings label `0x8a` "rx queue size" and `0x8b` "tx queue size";
those names are most plausibly from the **AOP's** perspective **[I]**. The
host-side truth is proven by the code: the TX action `fffffff005ba8398` writes
through the `+0x150` queue (the `0x8a` one), and the `0x85` handler schedules
the `+0xd0` queue (the `0x8b` one) into `routeEPICPacket`. Asahi's constants
(`0x8a = INIT_TX`, `0x8b = INIT_RX`) agree with the host view.

The queue constructor is `fffffff005baaf2c`:

```text
queue->endpoint   = provider endpoint object
queue->descriptor = buffer descriptor
queue->alignment  = slaveAlignment (0x40)
queue->offset     = descriptor offset
queue->base_va    = mapped_va + offset
queue->size       = descriptor size
queue->type       = 2 or 3
```

It requires `offset` and `size` to be multiples of the alignment, and
`offset + size <= buffer size`. Bounds are enforced by the caller as well.

### 4.3 Ring header (AP writes it)

The ring initializer is `fffffff005bab1c8`, called from the queue constructor
via `fffffff005b8465c`. With `block_size = slaveAlignment = 0x40`:

| Offset | Size | Content |
| --- | --- | --- |
| `+0x00` | 4 | `bufsize` = ring size − 3×0x40 (the payload area) |
| `+0x04` | 4 | marker `0x00070006` (fixed; written by the initializer) |
| `+0x40` | 4 | `rptr` = 0 |
| `+0x80` | 4 | `wptr` = 0 |
| `+0xC0` | `bufsize` | entries |

The initializer also zeroes the three header blocks. `bufsize` must be a
multiple of 0x40; the descriptor size is already in 0x40 units. If the DT ever
provides `aop-memory-alignment`, it replaces 0x40 and the three block offsets
become `0`, `block_size`, `2*block_size`; the j71t live DT does **not** have
that property.

### 4.4 Ring entry (EPIC packet inside the ring)

The j71t ring writer builds the entry as:

| Offset | Size | Content |
| --- | --- | --- |
| `+0x00` | 4 | magic: `"AOP "` (`0x20504f41`) is the only constant the host writer builds; the writer takes the alternative from an argument. Accept both `"AOP "` and `"IOP "` on RX. |
| `+0x04` | 4 | entry size: the writer's gather-length sum, used to advance the ring |
| `+0x08` | 8 | first 8 bytes of the EPIC outer header (version/seq, channel/reserved) |
| `+0x10` | up to `size` | rest of the EPIC packet: outer header bytes 8..15, then subheaders and payloads, followed by alignment padding |

The EPIC packet is split: the first 8 bytes are at `entry+8`, the remainder at
`entry+0x10`. The exact relation between the `+0x04` size and the EPIC length
is **[I]** until the first captured packet; do **not** derive the advance from
the subheader lengths. Match the writer's own advance:

```text
next_rptr = align_up(size, 0x40) mod bufsize
```

If the first capture shows the next magic at `align_up(0x10 + size)`, the field
excludes the 0x10-byte header; switch the formula then. The important part is
that writer and reader use the same rule.

For parsing, copy the 8 bytes at `entry+8` and a bounded window at
`entry+0x10` into one linear buffer, then walk the subpackets with their own
length fields. Accept the packet only if the walk ends inside the bytes you
actually copied (`8 + size` in the reference code); reject a walk that
overruns. This is robust to the exact size convention and to a ring that wraps
in the middle of an entry.

Evidence: the writer `fffffff005bab540` sums the gather-list lengths into the
u32 it stores at entry+4 (`stp w19,w26,[x21]`), stores the caller's first 8
EPIC bytes at entry+8 (`stp w9,w8,[x21,#8]`), and copies the rest at entry+0x10.
The only magic constant built in the kext is `"AOP "` (`movz #0x4f41; movk
#0x2050, lsl 16` at `fffffff005bab72c`, also at `fffffff005b847e8`); the
alternative value is caller-supplied. m1n1's equivalent writer uses `"IOP "`
for AP→IOP and accepts `"AOP "` on RX, but that constant is **not** present in
AppleSPU, so do not assume the host must write it. Write `"AOP "` first and
accept either on the IOP→AP ring; validate in the first capture.

---

## 5. RTKit management and AFK message sequence

### 5.1 Management dispatcher

`fffffff005b71018` is the endpoint-0 RX dispatcher. It extracts
`type = (word >> 52) & 0xf` and handles:

| Type | Direction | Meaning | Exact word / handler |
| --- | --- | --- | --- |
| 1 | IOP→AP | HELLO | `0x0010_0000_00MM_mm`; `fffffff005b7196c` |
| 2 | AP→IOP | HELLO_REPLY | `0x0020_0000_000c_000c`; `fffffff005b7196c` |
| 3 | AP→IOP | PING | `0x0030_0000_0000_0000 \| seq32 \| (uptime16 << 32)`; `fffffff005b71f1c` |
| 4 | IOP→AP | PING_ACK | `0x0040_0000_0000_0000 \| seq32`; dispatcher case 4 |
| 5 | AP→IOP | STARTEP | `0x0050_0000_0000_0002 \| (ep << 32)`; [I] (§5.4) |
| 6/7 | both | IOP power state | not observed on j71t; do not invent |
| 8 | both | EPMAP / reply | `fffffff005b71afc` |
| 0xa | AP→IOP | AP power/ready | `0x00a0_0000_0000_0000 \| 0/1`; `fffffff005b716d0` |
| 0xb | IOP→AP | AP power notification | dispatcher case 0xb, stores low32 at `+0xf0` |

The dispatcher also has a watchdog path: `fffffff005b71268` sends the type-3
ping every `rtb_ping_rate`-derived interval and expects the type-4 ack. If the
ack does not arrive within the configured window it logs
`"RTBuddy(%s): WARNING: ping timeout."`. A Linux driver should reproduce this
ping loop; the nub has `watchdog-enable`, and losing the pinger may let the
AOP reset the firmware (direction and timeout are not yet measured). Until the
exact interval is known, ping every 1 s and treat 5 s without an ack as a
transport failure (invalidate samples, stop the stream; do not reset the AOP).

### 5.2 HELLO

The IOP initiates. `fffffff005b7196c` requires:

```text
min = word & 0xffff;  max = (word >> 16) & 0xffff;
min <= 12 && max >= 12
```

and replies:

```text
0x00200000000c000c     (type 2, min = max = 12)
```

Bits 32/33 may be set for debugger/tracing; use 0. The `2062.142.1` firmware
release and protocol 12 are different version spaces.

On a version mismatch the handler builds an error hello/reply with the error
code in the low 16 bits (`fffffff005b71da0`); do not try to force a version.

### 5.3 EPMAP and endpoint creation

`fffffff005b71afc`:

```text
base   = (word >> 32) & 0x3f          // bits 37:32
last   = (word >> 51) & 1             // bit 51
bitmap = word & 0xffffffff
for each set bit i: create endpoint(base*32 + i)
reply = (0x8 << 52) | (last << 51) | (base << 32) | ack_low32
```

The reply low32 is `1` on the first chunk (`base == 0`); on later chunks iOS
sends a bitmap of endpoints that already exist and are running
(`fffffff005b71e08`). The exact device requirement for that bitmap is **[U]**;
always sending `(base == 0)` is a reasonable first implementation, but mirror
the iOS mask if the device retries.

Endpoint 0 is management. `RTBuddy::createEndpoint` (`fffffff005b661e4`)
special-cases system endpoint numbers 0,1,2,3,4,6,7,8,10,11; everything else
becomes an `RTBuddyEndpointService` named `AOPEndpoint<N>` (the `%sEndpoint%u`
string at `fffffff005315e1b`). `AppleSPU` matches those service names; the
accel is **not** a fixed endpoint constant.

The endpoint number is carried in the endpoint object and passed to
`RTBuddy::sendMessage` by the wrapper at `fffffff005b6a230`; that is also
where the wire high byte is set.

### 5.4 STARTEP

The iOS endpoint start path is `RTBuddyEndpoint`'s `+0xb0` method, called from
`createEndpoint` and from `AppleSPU::start` (`fffffff005ba7984`). The exact
instruction sequence that builds the type-5 word was not located in this round.
Use the protocol-12 layout that the same RTBuddy generation implements in
mainline:

```text
0x0050000000000002 | ((u64)endpoint << 32)
```

**[I]** until a live capture shows the first type-5 word. If the firmware
rejects it, the likely alternative is bit 1 cleared (`...0000`), but do not
guess without evidence.

### 5.5 AFK handshake (per endpoint)

The SPU/AFK switch is `fffffff005ba88e0`. It only accepts `0x80` before the
version is known:

| Type | Message direction | Meaning | Exact word |
| --- | --- | --- | --- |
| `0x80` | IOP→AP | AFK INIT (version 8) | `0x0080_0000_0000_0008` |
| `0xa0` | AP→IOP | INIT ACK | `0x00a0_0000_0000_0000` |
| `0x89` | IOP→AP | GET_BUF | `0x0089_0000_0000_0000 \| (blocks<<16) \| tag` |
| `0xa1` | AP→IOP | GET_BUF ACK | `0x00a1_0000_0000_0000 \| (pa & 0xffffffffffff)` |
| `0x8a` | IOP→AP | AP→IOP ring descriptor | `0x008a...` (§4.2) |
| `0x8b` | IOP→AP | IOP→AP ring descriptor | `0x008b...` (§4.2) |
| `0x8c` | IOP→AP | historical ring descriptor | `0x008c...` (§4.2) |
| `0xa3` | AP→IOP | START | `0x00a3_0000_0000_0000` |
| `0x86` | IOP→AP | START ACK | `0x0086_0000_0000_0000` |
| `0xa2` | AP→IOP | TX doorbell | `0x00a2_0000_0000_0000` (no wptr) |
| `0x85` | IOP→AP | RX notification | `0x0085_0000_0000_0000` (low bits ignored) |
| `0xa4` | AP→IOP | normal shutdown | `0x00a4_0000_0000_0000` |
| `0xc2` | AP→IOP | hibernate shutdown | `0x00c2_0000_0000_0000` |
| `0x87` | IOP→AP | shutdown ack (normal) | `0x0087_0000_0000_0000` |
| `0xc3` | IOP→AP | shutdown ack (hibernate) | `0x00c3_0000_0000_0000` |

Notes:

- The `0x80` value must have **exactly 8** in the low 48 bits; the switch
  panics/logs "Incompatible protocol version" otherwise.
- iOS **waits for the device's `0x80`**; it does not send `0x80` itself. The
  host gate drops everything except `0x80` until that message arrives. A Linux
  driver should wait, with a timeout; the Asahi-style "host sends 0x80 first"
  is **not** what this firmware expects. If a capture shows the device does not
  send it after STARTEP, revisit this, but do not preemptively send `0x80`.
- `0xa2` is a bare doorbell on j71t; the write pointer is read from the ring
  header, unlike modern AFK where it is carried in the message.
- The shutdown mapping (`0xa4`/`0x87` normal, `0xc2`/`0xc3` hibernate) is from
  the code at `fffffff005ba9808`: it calls `fffffff005ba9d54`, which returns the
  `IOHibernateState` value only when that property's state is 4, and selects
  `0xc2` only when the value is exactly 1; otherwise it sends `0xa4`. The
  switch treats both `0x87` and `0xc3` as the same wakeup.
- The `0x81`/`0x82`/`0x83`/`0x84` legacy paths exist in the switch but are not
  used by this firmware. Do not implement them.

### 5.6 Exact sequence to open the accel stream

1. Attach (§3) and process HELLO/EPMAP.
2. Send pings from now on.
3. For each app endpoint: STARTEP.
4. Per endpoint, wait for `0x80`; reply `0xa0`.
5. On `0x89`: allocate, reply `0xa1 | pa`.
6. On `0x8a`/`0x8b`/`0x8c`: create the ring, init the header, record the tag.
7. When both `0x8a` and `0x8b` exist: send `0xa3`; wait for `0x86`.
8. Read the IOP→AP ring on `0x85`; find the `0xc0` announcement whose name is
   `"accel"` (and service id `0x10000007`). Record the packet's channel.
9. Send the calibration properties in the table order (§7.2).
10. Send the rate properties (§7.3).
11. Decode `0xc2` reports (§8).

### 5.7 Differences from Asahi/m1n1 that matter

| Layer | j71t | Asahi/m1n1 |
| --- | --- | --- |
| Mailbox | 64-bit, endpoint bits 63:56 | ASC/96-bit, endpoint in `msg1` |
| Mgmt hello | IOP initiates; host replies | same direction in Linux RTKit; m1n1 also replies |
| EPMAP base | 6 bits (37:32) | 3 bits (34:32) in Linux |
| EPMAP endpoint | `base*32 + bit` | same formula, narrower base |
| AFK 0x8a/0x8b | 0x8a = AP→IOP, 0x8b = IOP→AP | same host view, opposite m1n1 names |
| Doorbell | bare `0xa2` | `0xa2 \| wptr` |
| Shutdown | `0xa4` / `0xc2` | `0xc0` |
| Shutdown ack | `0x87` / `0xc3` | `0xc1` |
| Ring entry | magic + entry size + split EPIC header | same family, newer header |
| Channel | per-interface number at +0x94; wire offset +4 **[I]** | channel in announce/header |

### 5.8 What is *not* the sequence

- Do not send a HELLO from the host; the device sends it.
- Do not send AFK `0x80` before the device does.
- Do not replay HELLO/EPMAP into a running firmware whose FIFO is empty.
- Do not treat endpoint 0 as the accel endpoint; endpoint 0 is management.
- Do not treat `service_id 0x10000007` as an RTKit endpoint or a queue channel.
  It is the HID service id in the announcement.
- Do not write the wptr into the `0xa2` message; the firmware reads the ring.

---

## 6. Services and channels

The ready announcement is a normal report packet on the IOP→AP ring:

```text
category = 0x00 (report), subtype = 0xc0
payload +0x00: name[32], NUL padded
payload +0x20: service_id u32
payload +0x24: flags u32
payload +0x28: optional interface number u32 (if flags bit 3)
```

`fffffff005ba8554` matches an existing interface by
`*(u32 *)(interface + 0x94) == channel`, parses the name, and creates an
interface with:

- `+0x90` = service id
- `+0x94` = channel
- `+0x98` = name string
- property `"service_id"` = service id

`fffffff005baefb4` is the constructor that proves those fields.

The **channel** is a separate per-interface number, not the service id and not
the endpoint. `fffffff005ba8398` passes `*(u32 *)(interface + 0x94)` to the
ring writer, and `routeEPICPacket` receives a channel and matches it against
`+0x94`. The 16-byte outer header has a spare u32 at `+0x04` that the builder
`fffffff005baf0fc` zeroes; the ring writer is the presumed place where the
interface channel is written **[I]**. Confirm the offset in the first capture;
the size is **not** stored there (the size is in the ring entry at +4).

The accel announcement is the one with name `"accel"`; the live ADT also gives
`service_id = 0x10000007`, `device-usage-page = 0xff00`,
`device-usage = 3`. Use the name as the primary key and the service id as a
consistency check. The announcement's channel is what all subsequent accel
commands and reports use.

The report descriptor request (subtype 1, `fffffff005b8d550`) is optional for
basic sampling. Get/set report use subtypes 6/7. Do not require the descriptor
before enabling the stream; iOS uses it to build HID elements, not to decode
the motion payload.

---

## 7. Commands: EPIC outer header, subheader, calibration, rate

### 7.1 Packet format

Outer header, 16 bytes (`fffffff005baf0fc`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | version = 2 |
| 1 | 2 | sequence (host counter) |
| 3 | 1 | pad = 0 |
| 4 | 4 | channel (host TX: interface channel; the builder writes 0 and the ring writer is the presumed writer of this field) **[I]** |
| 8 | 8 | timestamp (0 on host TX) |

Subheader, 24 bytes (`fffffff005bae308` / `fffffff005bae168`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | payload length (bytes after this 24-byte header) |
| 4 | 1 | version = 2 |
| 5 | 1 | category: report `0x00`, notify `0x10`, reply `0x20`, command `0x30` |
| 6 | 2 | subtype |
| 8 | 2 | sequence / transaction tag |
| 10 | 8 | AOP timestamp (unaligned; 0 on host TX) |
| 18 | 2 | reserved |
| 20 | 4 | capacity (`0xe4` for notify; 0 for command/report) |
| 24 | n | payload |

The property builder `fffffff005bae168` adds **4** to the caller's payload
length and copies the caller's bytes at subheader+28, leaving subheader+24..27
zero. So the on-wire payload is:

```text
00 00 00 00 | key (u32 LE) | value (n bytes)
length field = 4 + 4 + n = 8 + n
epic_len     = 16 + 24 + 8 + n = 48 + n
entry size   = see §4.4 (the exact relation is confirmed on the first packet)
```

This is the instruction-level confirmation of the round-1 "leading zero word".
Do not omit it and do not set the length to `4 + n`.

### 7.2 Calibration

`AppleSPUHIDDriver_sendFactoryCalibration` (`fffffff005b9d9e4`) walks the table
at `fffffff006d5e7f8`. The exact accel order and tags are:

| # | ADT property | Tag | Flags | Present on j71t |
| --- | --- | --- | --- | --- |
| 1 | `low-temp-accel-offset` | `0x1a` | 0 | yes, 20 B |
| 2 | `accel-sensitivity-calibration` | `0x0b` | 0 | yes, 36 B |
| 3 | `accel-range-sensitivity-cal` | `0x27` | 0 | no |
| 4 | `accel-orientation` | `0x18` | 0 | yes, 16 B |
| 5 | `accel-interrupt-calibration` | `0x19` | 0 | yes, 16 B |
| 6 | `accel-range-intr-cal` | `0x4b` | 1 | no |
| 7 | `accel-nominal-extr-cal` | `0x4c` | 1 | no |
| 8 | `accel-range-extr-cal` | `0x4d` | 1 | no |
| 9 | `accel-offset-cal` | `0x1a` | 1 | no |

Send only the present blobs, in this order, as subtype-4 notify packets:

```text
subheader: category 0x10, subtype 4, tag = seq, capacity 0xe4
payload:   00000000 | tag u32 | exact ADT bytes
```

The table's `flags` word is not part of the property key; the low 32 bits are
the key, the high 32 bits select the versioned/location-id handler for entries
6–9. Do not send an absent blob as zeros, and never write syscfg.

After the table, iOS may send calibration state key `0x3d` (32-bit). It is not
required for basic sampling; treat it as a separate, optional step and do not
invent a value.

**2026-10-25 correction:** the DFU cold stage-3 capture proves key `0x18`
returns `0xe00002c7` (`kIOReturnUnsupported`). Pinned firmware rejects it
unconditionally, and iOS `b96d70` tolerates this status while `ba0c34` does
not require orientation for completeness. Sending ARot does not prove it was
applied. The [stage-3 evidence and fix](aop-stage3.md#dfu-cold-key-0x18-rejection-root-cause-and-fix)
supersede the earlier expectation of four successful calibration ACKs.

### 7.3 Rate

The high-level setter `fffffff005b99ef0` shows the exact order and keys:

| Property | Key | Value |
| --- | --- | --- |
| Batch interval | `0x16` | u32, microseconds |
| Raw interval | `0x5c` | u32, microseconds |
| Report interval | `0` | u32, microseconds; 0 stops |

On enable, if batch is 0 the code substitutes 10000. On disable it zeroes
batch and raw, then sends `0x16=0`, `0x5c=0`, `0=0`. The default 10000 and the
Sleep/Wake behavior strongly indicate microseconds, but the unit and supported
rates are still **[U]** until measured. For a first autorotate stream use
`0x16 = 100000`, `0x5c = 0`, `0 = 100000` (10 Hz), then measure; do not
advertise a rate the firmware has not acknowledged.

Wait for the reply after each property (category `0x20`, tag equal to the
request's seq, first payload word = IOReturn). A timeout or nonzero IOReturn
must stop the enable plan, not proceed to the next property.

---

## 8. Reports: decode, scale, axes, ARot

`AppleSPU_routeEPICPacket` (`fffffff005ba83ec`) checks the outer version and
walks subpackets; `fffffff005bacde4` advances by `24 + length`.
`AppleSPUInterface_dispatchSubpacket` (`fffffff005bacf58`) accepts report
categories `< 0x10`; `AppleSPUHIDInterface_deliverReport`
(`fffffff005bad4d8`) passes the payload to the HID layer.

The accel report is category `0x00`, subtype `0xc2`
(`AppleSPUHIDDriver_dispatchMotionReport`, `fffffff005b9eb5c`). Payload after
the 24-byte subheader:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | sequence (u16) |
| 2 | 4 | motion type/status (u32) |
| 6 | 4 | X, signed |
| 10 | 4 | Y, signed |
| 14 | 4 | Z, signed |
| 18 | 4 | temperature (u32) |
| 22 | … | optional auxiliary |

The axes go unchanged into the iOS accelerometer event. `IOHIDEvent` declares
them as `IOFixed`, so the working interpretation is **signed 16.16 g**:

```text
scale = 9.80665 / 65536 = 0.000149637603759765625 m/s² per count
IIO: IIO_VAL_INT_PLUS_NANO, 0, 149638
```

The absolute g scale and the sign/orientation still need the six-pose check.
Do not export temperature; its scale is not established.

### 8.1 ARot

The live accel node has:

```text
accel-orientation = 01000000 ff000000 00ff0000 0000ff00   (16 bytes)
```

The plausible decoding is `u32 version = 1` followed by three rows of four
signed bytes with one pad byte per row: `(-1,0,0)`, `(0,-1,0)`, `(0,0,-1)`,
i.e. `-I`. Note that `det(-I) = -1`, so this is a handedness change, not a
proper rotation; do not assume it is a rotation matrix. The row/column-major
ambiguity does not matter for this symmetric blob, but the version/flags
parsing does.

The proven host behavior is: send ARot to firmware as property `0x18`, then
consume the axes without applying ARot on the AP. The pinned AOP image
**rejects** that property as unsupported, so firmware application is false;
the report frame still needs measurement. Therefore:

- **Mirror iOS**: send the per-device ARot blob, then expose a **measured
  residual screen mount matrix**. This is the recommended path.
- Do not also multiply by ARot in IIO.
- Do not treat the `1` version word as a matrix element.
- Do not hardcode `-I` as the mount matrix or put another unit's ARot in a
  public DTS.

Six poses (screen up/down, portrait top up/down, left/right edge up) give the
residual matrix and verify the signs. Until then, the driver must not register
usable IIO channels.

---

## 9. Close, pause, and power

- **Pause/resume**: set report-interval key `0` to 0; keep the endpoint and rings
  alive. Resume by sending the batch/raw/report trio again. This is what iOS
  does on sleep/wake for the ordinary stream.
- **Stream close**: send `0x16=0`, `0x5c=0`, `0=0`, then AFK shutdown
  `0xa4<<48`; wait for `0x87<<48` (or `0xc3`). The wait is on
  `AppleSPU+0x258` becoming nonzero (`fffffff005ba9808`).
- **Hibernate/long suspend**: iOS uses `0xc2<<48` instead; the device answers
  `0xc3`. Do not use it for an ordinary module unload.
- **Do not** send AFK shutdown merely because the display rotated or the
  stream paused; the endpoint is expensive to re-init and the firmware may not
  re-send `0x80`.
- **Unload**: if the module is removed while the AOP is still running, the
  ping loop stops and the watchdog may reset the firmware. Either keep a small
  keepalive owner outside the module or document that unload may reset the
  AOP. Never reset the AOP as an error handler.
- **Suspend**: only after stop/restart is repeatable. Preserve the PMGR
  dependencies from round 1; the ADT marks AOP always-on, but that is a
  policy hint, not a measurement of the cold state.

---

## 10. Driver recipe

### 10.1 State machine

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

### 10.2 Ring read (RX)

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

The copy is deliberate: the 16-byte EPIC header is split across `entry+8` and
`entry+0x10`, and the ring may wrap in the middle of an entry. Do not cast a
C struct over the ring.

### 10.3 Ring write (TX)

```c
entry = ring + 0xC0 + wptr;
/* size = bytes stored at entry+0x10; the first packet must confirm the
 * relation to epic_len (see §4.4). Build the entry, then write +4. */
u32 size = entry_size(epic_len);   /* writer's gather sum; see §4.4 */
put_unaligned_le32(0x20504f41, entry);          /* "AOP " */
put_unaligned_le32(size, entry + 4);
memcpy(entry + 8, epic, 8);                     /* EPIC bytes 0..7 */
memcpy(entry + 16, epic + 8, min(size, epic_len - 8));
wptr = align_up(wptr + size, 0x40) % bufsize;   /* writer rule */
writel(wptr, ring + 0x80);
dma_wmb();
send_endpoint_word(ep, 0x00a2000000000000);     /* bare doorbell */
```

`entry_size()` is the gather-length sum the writer stores at +4; until the
first capture, start with `epic_len` and validate the next entry's magic. The
copy is clamped to the EPIC remainder so an over-large field cannot read past
the packet; the parser then trusts the subheader lengths.

Before writing, check that the aligned entry fits between `wptr` and `rptr`
(wptr == rptr means empty; never let the new wptr catch rptr). A full ring is
a drop/retry, not a reason to overwrite unread entries.

### 10.4 Buffer request handler

```c
size = ((word >> 16) & 0xffff) * 64;
if (!size || table_count >= 16) return -EINVAL;
buf = dma_alloc_coherent(dev, ALIGN(size, 0x40), &pa, GFP_KERNEL);
if (!buf) return -ENOMEM;
/* pa must fit in 48 bits; the reply mask is 48 bits */
table[count++] = {tag: word & 0xffff, va: buf, pa: pa, size: size};
send_endpoint_word(ep, 0x00a1000000000000 | (pa & 0xffffffffffff));
```

Validate every 0x8a/0x8b/0x8c descriptor against the tagged buffer before
creating a ring. Reject `offset + size > buf_size`, unaligned values, or a
ring smaller than `0xC0 + 0x40`.

### 10.5 IIO exposure

- `in_accel_x_raw`, `in_accel_y_raw`, `in_accel_z_raw`: the reported s32
  counts.
- `in_accel_scale`: `IIO_VAL_INT_PLUS_NANO`, 0 and 149638.
- `in_accel_mount_matrix`: the measured residual matrix from six poses; not
  ARot, and not `-I` by default. If ARot was sent to firmware, the matrix is
  the **residual**; if it was not sent, the matrix is the full measured
  mapping. Never apply ARot twice.
- `in_accel_sampling_frequency`: read-only until the rate unit is measured;
  then `available` with the acknowledged rates.
- Provide a triggered buffer (hrtimer trigger at the report interval) because
  `iio-sensor-proxy` reads the IIO buffer. Timestamp with AP receipt time until
  the AOP timestamp domain is known.
- Return `-ENODATA` before the first sample and after a transport failure, and
  after `max(1 s, 5 × report interval)` without a sample; never return a
  fabricated zero-g sample.

`ipad-autorotate --probe` / `monitor-sensor` then verifies the physical
orientation. Accelerometer support alone does not rotate Hyprland; the
userspace policy still has to be enabled.

---

## 11. Evidence vs inference

### Proven from j71t artifacts

- Mailbox word format and endpoint high byte; send precondition
  (`fffffff005b66aa8`).
- Buffer request `0x89` → host IOKit buffer memory descriptor with 0x40 alignment →
  reply `0xa1 | physical address` (`fffffff005ba88e0`,
  `fffffff005b671b4`, `fffffff005b67264`).
- Queue descriptors `0x8a`/`0x8b`/`0x8c`, 64-byte units, tag/offset/size
  (`fffffff005ba88e0`, `fffffff005baaf2c`).
- Ring header: bufsize + `0x00070006` at +0, rptr at +0x40, wptr at +0x80,
  payload at +0xC0; AP initializes it (`fffffff005bab1c8`).
- Ring entry: magic + entry size + split EPIC header; the only magic
  constant the host writer builds is `"AOP "`, the alternative is an argument
  (`fffffff005bab540`, `fffffff005bab72c`).
- `slaveAlignment = 0x40` when `aop-memory-alignment` is absent
  (`fffffff005b4bb44`, `fffffff005b1f8ec`).
- Management hello (device-initiated, reply `0x00200000000c000c`)
  (`fffffff005b7196c`).
- EPMAP base/bitmap/last and endpoint formula (`fffffff005b71afc`).
- Ping type 3 and ack type 4 (`fffffff005b71f1c`, dispatcher case 4).
- AP power type 0xa with a 0/1 value (`fffffff005b716d0`).
- AFK `0x80`/`0xa0`, `0x89`/`0xa1`, `0x8a`/`0x8b`/`0x8c`, `0xa3`/`0x86`,
  `0xa2`, `0x85`, `0xa4`/`0xc2`, `0x87`/`0xc3`
  (`fffffff005ba88e0`, `fffffff005ba9754`, `fffffff005ba9808`,
  `fffffff005bab130`).
- 0x8a = AP→IOP ring, 0x8b = IOP→AP ring (TX action `fffffff005ba8398` uses
  +0x150; 0x85 handler uses +0xd0).
- Interface channel at +0x94, passed to the ring writer and matched on RX
  (`fffffff005ba8398`, `fffffff005baefb4`); the wire offset (+4) is [I].
- Ready report name/service id/flags/interface number
  (`fffffff005ba8554`).
- Property notify payload `00 00 00 00 | key | value`, length `8+n`, capacity
  `0xe4` (`fffffff005bae168`).
- Calibration table order/tags (`fffffff006d5e7f8`).
- Rate keys `0x16`, `0x5c`, `0` (`fffffff005b99ef0`).
- Report payload layout (`fffffff005b9eb5c`, `fffffff005bacde4`).
- ARot sent to firmware and not applied again on the AP
  (`fffffff005b9d9e4`, `fffffff005b9eb5c`).

### Inference (fits the evidence, not yet observed on hardware)

- STARTEP word layout (type 5, `ep<<32`, bit 1).
- Ring magic direction (`"AOP "` on host TX; accept both).
- AP power type 0xa value 1 at attach.
- Microsecond units for the rate keys.
- 16.16 g scale and the ARot `-I` decoding.
- EPMAP reply low32 semantics.
- Ping interval / watchdog window.

### Unresolved (must be measured or checked further)

- AOP state at our handoff (U1), especially the cold capture path.
- Region `0x9b000` vs `0xa0000`, firmware remap/patchbay, AP/IOP address
  relation for loading (U2).
- Whether the AOP sends AFK `0x80` spontaneously after STARTEP on a freshly
  loaded image, and whether a warm attach can ever be safe without a reset.
- The exact STARTEP instruction and flag on j71t.
- The rate unit, minimum/maximum, rounding and latency (U6).
- ARot blob semantics and the residual screen matrix (U5).
- Timestamp domain (U8).
- Ping cadence and watchdog timeout.
- Whether `0xa2` must ever carry a write pointer on this firmware (the code
  sends a bare doorbell, but the first capture should confirm).

---

## 12. Capture plan to close the remaining items

The existing observer is the right first step; fix the debugfs path first.

1. **Re-run the cold pass** with the documented path
   `/sys/kernel/debug/apple-a9-aop/status` (hyphens). The supplied
   `r2-cold-capture.txt` used underscores and therefore proves nothing about
   probe success. Record the boot path, the five wrapper values, and the raw
   outbox FIFO transcript if `cpu_running = 1`.
2. **Warm pass** with the same kernel/DT. Compare the running bit, fault bits
   and FIFO emptiness. A warm running bit with an empty FIFO is the "refuse"
   case, not an attach case.
3. **First active attach** only after a cold pass shows a pending HELLO or a
   stopped CPU that we can load. Log every TX/RX word with a monotonic
   timestamp and the endpoint number. The first transcript should show, in
   order: device HELLO, host reply, device EPMAP chunks, host replies, host
   STARTEP(s), device `0x80`, host `0xa0`, device `0x89`, host `0xa1`, device
   `0x8a`/`0x8b`/`0x8c`, host `0xa3`, device `0x86`, device `0xc0`.
4. **Ring layout check**: for the first RX packet, verify
   `magic ∈ {"AOP ","IOP "}`, `size <= bufsize - rptr`, that the subpacket
   walk ends inside the entry, and that the next entry at
   `align_up(size, 0x40)` starts with a valid magic. If the next magic is at
   `align_up(0x10 + size, 0x40)` instead, the field excludes the header; make
   writer and reader agree before enabling the stream.
5. **Rate check**: enable at 100000 µs, count reports over 10 s, and compare
   with 200000/50000. Record the reply IOReturn and the actual cadence.
6. **Six poses**: log raw axes plus `monitor-sensor` output for each pose;
   derive the residual matrix and the sign; only then register IIO.
7. **Ping**: log every type-3/type-4 pair and the interval; increase the
   interval until the first timeout to find the watchdog window.

Do not use the FIFO dump as a passive probe: reading `0x4038` consumes. Bound
it, log before acting, and never clear fault bits or reset the AOP.

---

## 13. Artifact index

| Path | Contents |
| --- | --- |
| `build/periph/aop-r3/ghidra/` | shared-server replies kept before it stopped answering |
| `build/periph/aop-r3/{spu,rtbuddy,iop,slave}-local*/` | offline decompilation/instruction listings |
| `build/periph/aop-r3/rtbuddy-asm*/`, `spu-asm*/` | raw instruction windows for the ring, channel and shutdown paths |
| `build/periph/aop-r3/kexts/IOSlaveProcessor.macho` | extracted slave-processor kext |
| `docs/periph/aop-sensors.md` | rounds 1–2: ADT, firmware, calibration, ARot, observer |
| `docs/periph/ds-als-hall.md` | evidence-tag style and the separate ALS/hall devices |
| `patches/aop/round2/0002-...patch` | existing protocol header and dormant IIO consumer |
| `build/boot/round4-source/drivers/soc/apple/rtkit.c` | mainline RTKit reference for type numbers |

No device command, no kernel build, no commit, and no kernel branch change was
made in this round.
