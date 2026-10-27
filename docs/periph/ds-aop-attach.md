# A9 AOP: fresh host attachment to a running firmware

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25 correction: **`+0xb88` is not verified CPU status.** It mirrors
inbox `+0x4008` in every saved boot today, including FIFO occupancy. Earlier
running/halted conclusions from its bit0 are withdrawn; execution remains
unknown. The current forced-cold path accepts RUN `+0x44` bit4 clear after
10 ms, with auto-boot/outbox disabled and fault-free mailboxes, under the
owner's accepted crash risk. See [aop-force-cold.md](aop-force-cold.md).
Historical captures and earlier path descriptions below retain their provenance.


Current forced-stop/cold-load implementation: [aop-force-cold.md](aop-force-cold.md).
It is a separate owner-authorized `aop_force_cold=1` opt-in; the historical
attach restrictions below still describe the other paths. Hardware untested.

Latest: [stage 2d–2h and fixed stage 3d](aop-stage2d.md), with bounded
owner-authorized control writes. Important correction: iOS +c004=0x11 /
+c024=1 is its NMI path, not ordinary mailbox wake; no entry executes it.

Current owner-authorized experiment: [aop-stage2b.md](aop-stage2b.md), following
the [stage-2 no-response result](../release/OMISSIONS.md). It
deliberately bypasses the identity/lifetime gates below under accepted crash
risk; the earlier refusal implementation described here is historical.

2026-10-25. Offline research and guarded implementation. No device access,
workspace/kernel commits, pushes, power/reset writes, or integration edits.
Kernel edits are uncommitted on `a9/aop`, Docker worktree `/src/wt/aop`, based
on `906da12f51cbd53bf8bc13df196281983f3fe5de`. Canonical source edits are under
`scripts/periph/aop/`. Device plan: [aop-attach-test-plan.md](aop-attach-test-plan.md).

Evidence labels: **[V]** inspected instructions/artifacts; **[D]** existing
device capture, not a new measurement; **[I]** inference; **[U]** unresolved.
All kernel addresses below are **unslid**, in iOS 16.7.10 / 20H350 j71t
kernelcache SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.

## Verdict and what changed

**The ordinary first-host protocol is reconstructable. A safe fresh-host
attachment to the observed survivor is not yet established.** [V/U] The
kernelcache contains the iOS **host**. Additional analysis of the pinned
firmware finds a HELLO-reply branch that can emit EPMAP again, without clearing
AFK allocations. Re-enumeration and ownership transfer are different steps.
The actual survivor's code identity and a path releasing all its old buffers
remain unproved. This is not a proof that stateless attachment is impossible.

The implemented experiment is `apple_a9_aop.attach_stage=1`: a frozen,
four-register mailbox **status** observation, with the existing five-domain
PMGR guard. Stages 2 (handshake only) and 3 (AFK/accel) have explicit
`refused-no-verified-host-epoch` gates and do **nothing to hardware**. They
are reserved interfaces, not working handshake or sampling modes. This keeps
the known running instance intact while making the next safe observation
concrete. No accelerometer sample or successful attach is claimed.

Useful corrections to older notes were verified this round:

* The management vocabulary includes **AP→IOP type 0xb power notifications**
  (`fffffff005b71504`), in addition to type 0xa readiness and type 6 IOP power
  requests. The old “complete vocabulary” tables omitted them.
* Type 5 has **start=2 and stop=1**, not just start (`fffffff005b717a4`).
  Neither form establishes AFK-buffer release by a foreign firmware session.
* `%sEndpoint%u` uses **wire endpoint minus 0x1f**: `sub w19,w1,#0x1f` at
  `fffffff005b6a914`, stacked for the formatter at `fffffff005b6a948`.
  Thus saved `AOPEndpoint2` means wire **0x21**, not 2 or 0x22. [V] Discover
  it anew; `service_id=0x10000007` and the ring channel are separate identifiers.
* In the pinned receiver, type2 can reset the **map cursor** and send another
  EPMAP. That does not reset the AFK allocation state (§3.2). The old claim
  “an EPMAP response proves no earlier host existed” is therefore unsafe.

The ring-location study's AP-initialization claim also needs qualification:
the common initializer can initialize a ring, but the AppleSPU constructor
passes **init=0**. The later Round 3 correction in `aop-sensors.md` is right.
Do not zero an inherited ring or initialize a new ring ahead of the firmware.

## 1. Transport and the iOS first-host sequence

### 1.1 Mailbox, not ASC's 96-bit interface

**[V]** Wrapper base `0x210800000`, span `0x1c000`. The A7IOPV2 accessor at
`fffffff005b4e498` performs one 64-bit load of outbox data `+0x4038`;
`fffffff005b4e4a8` performs one 64-bit store to inbox data `+0x4010`.
The former **consumes** a FIFO word. Inbox status `+0x4008` and outbox status
`+0x4020` are 32-bit: full bit 16, empty bit 17, faults bits 18/19; outbox
enable is bit 0. The vendor `+0xb88` bit0 predicate is unverified as CPU status. AIC 133 is RX; 130 is TX space.

`RTBuddy::sendMessage` `fffffff005b66aa8` encodes:

```text
wire = (endpoint << 56) | payload56
endpoint 63:56; management type 55:52; AFK type 55:48
```

These are payload formulas below; put application endpoints in the high byte.
Endpoint 0 carries management. Do not insert an ASC `msg1` word or translate
an AFK buffer PA through the firmware remap aperture.

### 1.2 HELLO, EPMAP and readiness

| Order | Direction / payload | Evidence and conditions |
| --- | --- | --- |
| 1 | IOP→AP `(1<<52) \| (max<<16) \| min` | `handleHello` `fffffff005b7196c`; host IOP state must be **4 or 0x8000**. Range must contain 12. |
| 2 | AP→IOP `0x00200000000c000c` | Same handler, after changing host state to **5**. iOS can OR debugger/tracing flags into bits 32/33; Linux uses zero. No host-originated HELLO. |
| 3 | IOP→AP `(8<<52) \| (last<<51) \| (base<<32) \| bitmap32` | `handleEndpointMap` `fffffff005b71afc`, accepted only in state **5**. `base=(word>>32)&0x3f`; each bit names `base*32+i`. |
| 4 | AP→IOP `(8<<52) \| (last<<51) \| (base<<32) \| ack32` | During the initial map, `ack32=(base==0)` for **each** chunk. Once its complete flag `management+0x109` is set, repeated maps instead use the **existing running-endpoint bitmap** from `fffffff005b71e08`. This is not unconditional endpoint recreation. |
| 5 | AP→IOP `(0xa<<52) \| 1` | `fffffff005b716d0`; queued until state >=6. Registration path `fffffff005b668ec` requests readiness=1; the last EPMAP reply advances to state 6 and flushes a pending request. Thus this value has a real iOS caller, rather than being only a Linux guess. |
| 6 | IOP→AP `(7<<52) \| ... \| 0x20` | Type-7 dispatcher, within `fffffff005b71018`, advances host state to **8** (boot complete). iOS boot waits for state 8 or failure 0x8000; default timeout is 20 s. |
| 7 | AP→IOP `(5<<52) \| (ep<<32) \| 2` | Endpoint control builder `fffffff005b717a4`; state >=4. The alternate low word **1** stops an endpoint. |

**Ordering qualification [V]:** iOS creates and starts recognized **system**
endpoints while processing EPMAP (`fffffff005b661e4`); application endpoint
services start when their clients bind. There is not a single globally serial
“all maps, then start every endpoint” trace. A staged Linux handshake must
explicitly suppress these starts to avoid system-endpoint buffer allocations.
The existing active Linux driver deliberately skips system endpoints; that
is a remaining behavioral difference, not a demonstrated equivalent boot.

**[V]** Ping sender `fffffff005b71f1c` builds
`(3<<52) | ((mach_absolute_time() & 0xffff)<<32) | seq32` and increments its
TX counter on success. RX type 4 matches the separate counter at +0xf8 and
increments it. Initialization `fffffff005b71018` uses TX=0x15/RX=0x16; the
power action `fffffff005b713f0` can reset **both host counters to zero**.
These are watchdog counters, **not firmware epochs**. The driver's echo/+1
tolerance covers the two observed host initializations; a ping ACK proves
liveness only. The older description of the high field as seconds is not
what this iOS sender does. No new ping implementation is added here.

### 1.3 Power messages that resemble wake/reinit

All exist in the host code [V]; none has established foreign-session cleanup:

| Message | Exact sender | Meaning / limitation |
| --- | --- | --- |
| type 6 requests | `fffffff005b71750`, table `fffffff0053110a0` | Four full words `0x0060000000000201`, `...0220`, `...0202`, `...0000`, selected by state 0..3. Ordinary IOP power-state transitions. |
| quiesced→boot | `fffffff005b71fc4`, caller `fffffff005b65b54` | Sends `0x0060000000000220` only when the old status is 3. Same power-table word, not a distinct reinitialize command. |
| type 0xa | `fffffff005b716d0` | Host readiness boolean, defers until IOP state >=6. |
| **AP→IOP type 0xb** | `fffffff005b71504`, callers in `fffffff005b713f0` | Host state 3→payload0, 1→0x20, 0→0x201; sends `(0xb<<52)|payload`, waits for its cached response. RX type 0xb stores low32 and wakes the waiter. This path uses the existing RTBuddy object. |

`fffffff005b715d4` quiesces the **host watchdog** by waiting for outstanding
pings. `fffffff005b71e7c` checks watchdog timeout and calls error handling;
neither resets a firmware epoch. “Wake message exists” is true in the ordinary
power-management sense; “wake means forget all old buffers” remains **[U]**.

### 1.4 AFK endpoint initialization and the 0x89 allocation

**[V]** AppleSPU switch `fffffff005ba88e0`, start `fffffff005ba9754`, stop
`fffffff005ba9808`:

```text
IOP -> AP ep E: (0x80<<48) | 8            INIT, low48 must equal exactly 8
AP  -> IOP E:  (0xa0<<48)                INIT_ACK
IOP -> AP E:  (0x89<<48) | (blocks<<16) | tag16
AP allocates contiguous, coherent, Linux-owned backing, size=blocks*64
AP  -> IOP E: (0xa1<<48) | pa48          no tag in this reply
IOP -> AP E: (type<<48) | (off_blocks<<32) | (size_blocks<<16) | tag16
              type 0x8a = AP->IOP ring; 0x8b = IOP->AP; 0x8c = historical
AP validates each descriptor and the firmware-written ring header
AP  -> IOP E: (0xa3<<48)                 START, once TX+RX are present
IOP -> AP E: (0x86<<48)                  START_ACK
IOP -> AP E: (0x85<<48)                  RX notification; read ring
AP  -> IOP E: (0xa2<<48)                 bare TX doorbell when needed
```

In the switch's 0x89 case (`fffffff005ba8bb4`), the provider's +0x558
allocator is `fffffff005b671b4` → action `fffffff005b67264`. The buffer's
physical address is accessor +0x80; reply masks it to 48 bits. There is no
AOP DART in this saved board's path. iOS rounds backing to its allocation
granule, while the table retains the requested `blocks*64` extent. Alignment
is at least 64 bytes; queue descriptors must fit the requested extent.
Linux must verify **direct DMA** and the whole PA extent fits 48 bits before
publishing it. Reject zero/oversize/duplicate tags and overlapping rings.

The switch has a 16-buffer bound. AppleSPU's table is count at +0x508,
entries at +0x288 with stride 0x28: tag, buffer object, descriptor, **PA**,
requested size. `+0x10` within an entry is a descriptor, not a kernel VA
([ring-location correction §8](ds-aop-ring-location.md#82-the-table-corrected)).

Normal AFK shutdown is `0xa4<<48` → `0x87<<48`; hibernate variant is
`0xc2<<48` → `0xc3<<48`. On wake, `0xa3` restarts the queues. The stop path
waits for +0x258 and drains stale packets with `fffffff005ba84d0`; it does
**not** reallocate its table, send a new 0x89 reply, or zero its queue objects.
This is positive evidence of **same-buffer** resume, not stateless attach.
It still does not prove all firmware DMA stops even when an ACK arrives.

## 2. Ring and sample formats

### 2.1 Header and ownership

**[V]** Queue constructor `fffffff005baaf2c` → wrapper `fffffff005b8465c` →
initializer `fffffff005bab1c8`. Constructor passes init=0; initializer's
zero/write branch runs only with init!=0. On this host path it checks the
existing size and adopts the indices. With alignment A=0x40:

| Ring offset | Field |
| --- | --- |
| +0x00 | u32 capacity = descriptor size − 0xc0 |
| +0x04 | version marker; common initializer builds `0x00070006`; v7 consumer tests high half==7 |
| +0x40 | u32 rptr, consumer-owned |
| +0x80 | u32 wptr, producer-owned |
| +0xc0 | data, indices relative to this start |

Saved IOService queues are 0x4000 each, capacity 0x3f40 [V artifact]. This is
not a fixed firmware carveout. The firmware must initialize fresh shared
headers before announcing them. AP writes **RX rptr**, **TX entries/wptr**
only after ownership is established; it never zeros the header to force init.

### 2.2 Entries and EPIC

**[V]** Reader `fffffff005baba00`, writer `fffffff005bab540`, commit-read
`fffffff005babd00`, channel TX `fffffff005ba8398`:

```text
entry +0   u32 magic (default "AOP "; caller can supply another)
      +4   u32 payload length N (EXCLUDES the 16-byte entry header)
      +8   u32 channel
      +12  u32 auxiliary word (host writes 0)
      +16  complete EPIC packet, N bytes
stride = align_up(16+N, 64)
```

On wrap, the writer puts the whole entry at data offset 0 and leaves a copy
of the 16-byte header at the old tail position. Reader uses that header's
length to select offset 0. It is **not** a split EPIC header or split payload.
After ordered publication of wptr, the writer re-reads rptr and rings the
bare 0xa2 doorbell when old wptr==rptr. RX commits rptr after processing;
no per-entry RX mailbox ACK is required. Preserve DMA barriers at both ends.

EPIC outer header is 16 bytes: version2@0, seq16@1, zero/reserved u32@4,
timestamp64@8 (`fffffff005baf0fc`). Channel is in the **ring entry**, not that
reserved word. Each following subheader is 24 bytes: payload length32@0,
version2@4, category8@5, subtype16@6, transaction tag16@8, timestamp64@10
(unaligned), reserved16@18, capacity32@20. Walk by 24+payload length.

Ready report category0/subtype0xc0 has name[32], service_id32@32, flags32@36,
optional interface number32@40 when flag bit3 is set (`fffffff005ba8554`).
Bind name `accel` plus ID `0x10000007`; retain its endpoint and entry channel.

### 2.3 Calibration and sensor enable

**[V]** Property notify builder `fffffff005bae168` and setter
`fffffff005bad898`: category0x10/subtype4, capacity0xe4,
payload `le32(0) | le32(key) | exact value bytes`; payload length is 8+N.
Correlate category0x20 replies by transaction tag and check IOReturn==0.

Calibration walker `fffffff005b9d9e4`, table `fffffff006d5e7f8`:

| Order | Property | Key | Saved j71t size |
| --- | --- | --- | --- |
| 1 | low-temp-accel-offset | 0x1a | 20 |
| 2 | accel-sensitivity-calibration | 0x0b | 36 |
| 3 | accel-range-sensitivity-cal | 0x27 | absent |
| 4 | accel-orientation | 0x18 | 16 |
| 5 | accel-interrupt-calibration | 0x19 | 16 |
| 6..9 | range-intr / nominal-extr / range-extr / offset | 0x4b / 0x4c / 0x4d / 0x1a | absent; flagged selection paths |

Use this device's existing private dump; skip absent blobs, preserve order.
Do not invent calibration-state key0x3d or write factory storage. iOS sends
ARot to firmware and consumes the returned axes unchanged; do not apply ARot
again on the AP.

**[V]** Rate setter `fffffff005b99ef0` sends batch key0x16, raw key0x5c,
report key0, each a u32, in that order. Report=0 stops ordinary samples.
The conservative candidate is 100000/0/100000 (10 Hz **if** units are µs).
Units, accepted rates and actual cadence remain device-validation items;
they are not established by a successful property reply alone.

### 2.4 Samples

**[V]** `AppleSPUHIDDriver_dispatchMotionReport` `fffffff005b9eb5c` handles
accel category0/subtype0xc2. At least 22 payload bytes:

```text
seq u16@0; status/type u32@2; X/Y/Z s32@6/10/14; temperature u32@18
```

Axes pass directly to the accelerometer event factory. Signed 16.16 **g**
is the supported interpretation [I]; proposed IIO scale is
`9.80665/65536 = 0.000149637603759765625 m/s²/count`. Physical magnitude,
six-pose signs and the residual screen mount matrix must be measured before
enabling rotation policy. Temperature units and AOP timestamp conversion
remain [U]. Use explicit unaligned, bounded parsing, never structure casts.

## 3. Can this running instance take a fresh host?

Stage-2b qualification: the pinned type-2 re-enumeration branch belongs to
the handshake receiver `010311ac`. The separate runtime dispatcher at
`0103131c` does not call the EPMAP emitter for type 2. Re-enumeration therefore
depends on firmware control state. The new [study](aop-stage2b.md) traces the
mailbox enable helpers and management sleep/wake transition, and builds a
bounded diagnostic ladder plus an explicit optional 201→220 cycle. No claim
is made that either transition releases AFK allocations.

### 3.1 Startup provenance is still an inference

The saved power-off experiment
[logs-aop-state-20261024.txt](../release/OMISSIONS.md) shows **running at
Linux probe**, PMGR target/actual=f, CPU `0x00020001`, outbox enable=1 [D].
It excludes “ordinary slide-to-power-off reliably leaves a halted core for
this boot chain.” It does not identify the running code or its last loader.

Per [running-image study](ds-aop-running-image.md), the manifest marks AOP
not iBoot-loaded; the inspected iBoot path reads eight region words rather
than copying firmware, and the pongoOS/m1n1 path has no AOP loader. The best
supported explanation remains a surviving instance last started by iOS
AppleSPUFirmwareService → RTBuddy → AppleA7IOP [I]. Do not relabel that as an
observed BootROM/bootloader action. A very early hardware action remains
unexcluded by a post-PMGR snapshot.

The AP region's known bytes do not identify the executing image. Published
iOS FirmwareUUID/Version identify the **parsed file**, and the observed remap
upper bound 0x1ff000 does not authorize a wider aperture. Existing identity,
ext reservation and stale-memory refusals remain unchanged.

### 3.2 Re-enumeration exists; no established epoch reset

`handleHello` accepts in host boot state 4 or failure state 0x8000; EPMAP
requires state5. Its repeated-map branch deliberately reports **retained
running endpoints**. There is no nonce, host-generation token, or demonstrated
“forget all rings” exchange in the traced host handlers. Clearing Linux's
local state or its ping counters changes nothing on the IOP [V/I].

**New receiver evidence [V-file, not device]:** main firmware SHA256
`af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8` was
analyzed separately, without changing the shared kernelcache program. The
following are **linked Thumb VAs**, not AP PAs:

* `0x010311ac` is the queued management-message parser, tied to an event
  callback by setup at `0x0103474e`. Its switch at `0x010311e0` extracts the
  management type. Type2 enters **0x010311f6**, checks min==max==12, and
  reaches **0x0103128a**: state=3, map cursor at object+0x38=0, save flags,
  call **0x01031538**. No previous-state check protects this case.
* `0x01031538` enumerates the **existing endpoint store** and emits type8
  chunks. It does not traverse AFK buffers, clear old PAs or wait for DMA.
  Thus even a genuine EPMAP after HELLO_REPLY cannot serve as a “first host”
  certificate. This is a concrete counterexample to the old adoption rule.
* The fast handler **0x0103131c**, installed at `0x01034762..64`, handles
  type5/6/0xa/0xb directly. Type2 goes through its default at **0x01031378**,
  signals the queued event and returns 0. This supports dispatch to the
  queued parser, rather than proving that type2 is ignored after boot.
  Full runtime scheduling on the actual survivor remains unmeasured [I/U].
* AFK initializer **0x0100f5dc** is a separate state machine at object+0xac:
  0 sends INIT8, 1 waits for 0xa0, 2 sends 0x89 (tag **0xcafe**), 3 accepts
  0xa1 and records PA low/high at +0xb8/+0xbc, then maps it at +0xc0.
  Later states construct and announce rings. A repeated 0xa0/0xa1 while
  already in terminal init state8 does **not** execute the PA-accept branch.
* AFK dispatcher **0x0100f864** maps 0xa3→start ACK0x86 and
  0xa4→stop ACK0x87 without resetting init state+0xac. It preserves the
  buffer/ring mapping. The internal endpoint setup **0x0100f27c** can reset
  init state at **0x0100f368**, so endpoint teardown/recreation is a useful
  next research target. There is still no demonstrated management sequence
  that reaches it while draining every old client and master transaction.

> Private graphics-dependent excerpt omitted from this source snapshot.

`AppleA7IOP::start` `fffffff005b4b738` uses its own started flag and can return
busy; it does not adopt an unknown running hardware CPU. RTBuddy transition
`fffffff005b65b54` treats busy as an assertion/panic, not a takeover branch.
The DT `running` property (`fffffff005b68764`, loader `fffffff005b6f8a4`)
alters image/patch work; it supplies no AFK address-transfer protocol.
Hibernation path around `fffffff005b64bd0` retains/restores AP objects and
changes host state directly. Linux has no such restored object graph.

An unsolicited HELLO could be first boot, a retransmission, or a firmware
restart after earlier buffer publication [U]. An EPMAP after a duplicate
HELLO_REPLY can be a replay [V-file; actual survivor response U]. **Neither alone proves old DMA references
were invalidated.** The older Round 4 “EPMAP proves adoption/no old DMA”
inference is insufficient for a safe survivor attach. Its speculative path
is not reused or enabled by the new selector.

### 3.3 What must be learned next

The missing proof is now narrower: connect type5 stop/start and any internal
endpoint teardown to **all** AFK PA-table invalidation and in-flight DMA drain,
while preserving firmware ext storage. The pinned type2 re-enumeration and
ordinary AFK stop/start do not supply it. Trace that lifecycle in an
independently identified firmware image, or obtain a valid original-owner
handoff preserving/reserving every referenced buffer. Ping, AP-power ACK,
AFK stop ACK, an empty FIFO, and rate0 are individually insufficient global
DMA-release proofs.

Until then: **stage1 is a safe diagnostic candidate under the existing
powered-MMIO assumptions; actual stages2/3 are not safe to try on this known
survivor.** Their fail-closed implementations can be tested as refusals.

## 4. Staged implementation and abort contract

`attach_stage` is immutable uint, default0, observer-owned. Values >3 fail
probe. Any nonzero stage blocks the active driver **before allocations or
resource access**, even with active=1. Staged mode conflicts with debug,
state_probe, attach, ring_scan, ring_iio and ring_pa/ring_pa_size. No DT changes.
The postcore ring-capture entry also exits before any RAM capture when a
nonzero stage is selected, including on a conflicting command line.

| Stage | Implemented behavior | Abort / safety boundary |
| --- | --- | --- |
| 1 | Map only the four existing 4-byte status resources; guard; freeze one result in 0400 `attach_status` and dmesg. No CPU-control mapping, FIFO data, IRQ, DMA, timer, firmware/ext access. | Missing provider defers without wrapper reads; domain/error skips wrapper; faults, stopped CPU, disabled outbox report aborted. Pending FIFO is **unconsumed** and ends observation. Empty means no word available then, not protocol ownership. |
| 2 | **Refusal only**, before mappings or PMGR reads. | `refused-no-verified-host-epoch`. Future handshake must prove old-address release and identify the image before consuming even one word. |
| 3 | **Refusal only**, same early boundary. | Same refusal; publishing a fresh PA is prohibited without stage2 ownership proof. |

The existing active driver is unchanged except for the new early exclusion.
`attach_stage=0` preserves its old separate cold/speculative behavior; this
report does not recommend those paths on the observed survivor.

Future stage2, **after** resolving §3.3: one attempt, bounded receive budget,
HELLO deadline5s/EPMAP2s, version12 only, ordered unique bounded EPMAP chunks,
endpoint<=255, AP readiness only if required by the proven receiver contract.
Stop before **all** STARTEP, including system endpoints. Do not run a background
ping loop after the experiment. Abort on faults, nonempty unexpected traffic,
duplicate HELLO/map or timeout; freeze transcript, do not flush/retry/reset.
A stage2 failure after a protocol mutation is not rolled back by kexec.

Future stage3: same newly proved handshake, explicitly bounded endpoint set;
start/AFK8/0x89 with fresh direct coherent allocations, validate headers and
descriptors, receive accel announcement, send this unit's calibration then
batch/raw/report with correlated replies, validate first samples. Require a
total buffer budget, unique tags, bounds and nonoverlap, ordered DMA, and a
bounded packet/notification budget. Abort on any bad request, reply, index,
packet, deadline or liveness failure. **Keep every published buffer pinned**
on partial failure; rate0 or a shutdown ACK alone cannot authorize freeing
it or allowing the next kernel to reuse it. No reset/power/mask/remap/image/tag
writes at any stage. The retained firmware's **ext** allocation needs its own
lifetime proof too; allocating fresh AFK rings does not fix stale ext data.

## 5. Reproduction, validation and files

Run `python3 -B scripts/periph/aop/capture-attach-re.py` to capture the shared
program identity and 46 focused entries under `build/periph/aop-attach/re/`.
It verifies the kernelcache hash and uses `scripts/ghidra.py`; it does not
switch/restart the shared program. Missing function definitions are recorded
as errors plus bounded bytes. Fresh local extracted-kext fallback this round:

```sh
~/.local/share/uv/tools/ghidra-mcp/bin/python -B scripts/periph/ghidra-local.py \
  --binary build/periph/aop-r1/kexts/AppleSPU.macho \
  --output build/periph/aop-attach/spu-local \
  --function 0xfffffff005ba88e0 --function 0xfffffff005baaf2c
~/.local/share/uv/tools/ghidra-mcp/bin/python -B scripts/periph/aop/firmware-attach-re.py
```

The other missing entries use the existing extracted-kext instruction
evidence cited in the source studies; their absence from Ghidra's function
table is not absence of code. The critical endpoint-name subtraction is in
the **shared disassembly**, because the variadic decompiler hides it.
`firmware-attach-re.py` SHA-pins the main image, wraps unchanged bytes at
linked VA 0x01000000 and analyzes selected Thumb entries in its own ignored
Ghidra project. It also decodes management/AFK jump tables. It never changes
the shared kernelcache program. Decompiler prototypes/stack reconstruction
remain imperfect; branch claims above were checked against instructions.
Four understood host functions were renamed/commented in the shared project:
`b717a4`, `b71750`, `b71504`, `b6a904`.

Validation: ASan/UBSan production-function host tests, W=1 AOP object compile,
full arm64 Image.gz/DTBs, linked/template/DT checks, and checkpatch. Exact
results and host/device commands are in the companion test plan. These test
software guards and parser behavior, not firmware acceptance or hardware DMA.

Changed files owned by this task:

* `docs/periph/ds-aop-attach.md`
* `docs/periph/aop-attach-test-plan.md`
* `scripts/periph/aop/capture-attach-re.py`
* `scripts/periph/aop/firmware-attach-re.py`
* `scripts/periph/aop/check-build.py`
* `scripts/periph/aop/install-kernel.py`
* `scripts/periph/aop/test-offline.py`
* `scripts/periph/aop/tests/state-probe.c`
* `scripts/periph/aop/kernel/a9-aop.h`
* `scripts/periph/aop/kernel/a9-aop-ring-reader.c`
* `scripts/periph/aop/kernel/apple-a9-aop.c`
* `scripts/periph/aop/kernel/apple-a9-aop-active.c`
* `scripts/periph/aop/kernel/apple-a9-aop.rst`

> Private graphics-dependent excerpt omitted from this source snapshot.

