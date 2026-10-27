# Attaching to a running j71t AOP (round 4)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25 correction: **`+0xb88` is not verified CPU status.** It mirrors
inbox `+0x4008` in every saved boot today, including FIFO occupancy. Earlier
running/halted conclusions from its bit0 are withdrawn; execution remains
unknown. The current forced-cold path accepts RUN `+0x44` bit4 clear after
10 ms, with auto-boot/outbox disabled and fault-free mailboxes, under the
owner's accepted crash risk. See [aop-force-cold.md](aop-force-cold.md).
Historical captures and earlier path descriptions below retain their provenance.


2026-10-23. Research only: **no device access, no kernel edits, no commits** were
made for this document. Everything below comes from the saved ADT planes, the
shared iOS 16.7.10 kernelcache (`build/kc/20H350__iPad6,11/`), the extracted
kexts in `build/periph/aop-r1/kexts/`, the m1n1 tree in the `linux-a9-src`
volume, the upstream pongoOS tree, the AOP firmware in
`build/periph/aop-r2/firmware/apple/aop/`, and the two live captures:

| Capture | Boot | Driver result |
| --- | --- | --- |
| `build/periph/aop-r4/live/active-20261023T154929Z/` | kexec warm (8th+) | `refused-running-no-pending-hello (-16)` |
| `build/periph/aop-r4/live/active-20261023T160345Z/` | **true cold** (DFU → checkm8 → pongoOS → m1n1 → Linux, `kexec_marker=absent`) | `refused-running-no-pending-hello (-16)` |

Evidence tags as in [`ds-als-hall.md`](ds-als-hall.md): **[E]** read from an
artifact (code, ADT, firmware, source, live register), **[I]** inference that
fits the evidence, **[U]** unresolved. Kernel addresses are unslid virtual
addresses in the same kernelcache as `ds-aop-accel.md`.

---

## 0. Result in one table

| Question | Answer | Tag |
| --- | --- | --- |
| Who started the AOP? | Not our chain. The AOP instance is **inherited**: an AP-side iOS boot (AppleSPUFirmwareService → RTBuddy → AppleA7IOP) loaded `ipad6faop` and released the CPU, and the always-on AOP island kept it running through every iOS exit, DFU entry and kexec since. The cold boot proves it: the pre-Linux register state is **identical** to the warm boot's. | [E] state, [I] who |
| Did iBoot start it *on this cold boot*? | No. The palera1n payload chain never executes iBoot (`autoboot.sh`: palera1n → pongoOS → `/send m1n1` → `bootm`), pongoOS has no AOP/IOP driver, and m1n1's only IOP loader (AKF) refuses a firmware node without `pre-loaded`, which j71t does not have. | [E] chain + code |
| Does the cold/warm PMGR difference mean anything? | No. `auto=1` on the cold boot is **our own** `pmgr` cold path writing `APPLE_PMGR_AUTO_ENABLE`; the raw pre-Linux value is `0x0f0000ff` (AUTO=0) on **both** boots. | [E] |
| What would a true power cycle show? | Domains OFF, `cpu_running=0`, outbox empty → the cold load path becomes possible, but the current driver would stop at `refused-domain-state` because it refuses to power a domain on. | [I] |
| Does iOS send a takeover/wake message to a running IOP? | **No.** The only "already running" handling is (a) a DT `running` flag that skips the tag/version work, and (b) a **panic** when the provider returns `kIOReturnBusy`. iOS's real warm attach is *hibernation state restoration*, which needs the AP-side objects (endpoint map, rings) restored from the hibernation image — we have no such image. | [E] |
| Complete AP→IOP management vocabulary | HELLO_REPLY `0x00200000000c000c`, PING `0x0030\|uptime<<32\|seq`, STARTEP `0x0050\|ep<<32\|2`, EPMAP reply `0x0080\|...`, AP power `0x00a0\|on`, (warm only) boot-complete request `0x0060000000000220`. Nothing else exists. | [E] |
| Can we attach safely? | Yes, but only to a firmware that **never completed the HELLO handshake** and whose region fingerprint + ext segment still match what we would load. That is exactly the "iBoot/early-AP started it, nobody listened" case. A firmware that a previous AP already handshook must stay refused. | [E]+[I] |
| Does the running firmware need our calibration/endpoints? | In the adoptable case **yes, everything**: EPMAP replies, AP power, STARTEP, AFK init, GET_BUF, rings, ready report, factory calibration, rate. In the refused case we cannot know and must not guess. | [I] |
| Found defect | The driver's ping-ack check is off by one against iOS: iOS expects `sent_seq + 1`, the driver requires an exact echo. On a healthy AOP the driver would log `failed-ping-timeout` after 5 s. | [E] |

---

## 1. Q1 — who started the AOP, and what the cold boot settled

### 1.1 The two snapshots are the same snapshot

| Register | Warm (`…154929Z`) | Cold (`…160345Z`) |
| --- | --- | --- |
| `0x210800044` cpu_control | `0x00000000` | `0x00000000` |
| `0x210800b88` cpu_status | `0x00020001` | `0x00020001` |
| `0x210804008` inbox ctrl | `0x00020001` | `0x00020001` |
| `0x210804020` outbox ctrl | `0x00020001` | `0x00020001` |
| `0x210804040` idle status | `0x00000079` | `0x00000079` |
| faults | 0 | 0 |

Both are `hardware_writes=none` ([E], `status.txt` of each capture).

The PMGR trace settles the `auto` bit question. On the cold boot, at
`boottime 0.08 s – 2.24 s` and **before any write**:

```text
PMGR aop        raw before floor @80000 reg=0f0000ff warm=0 hold=0
PMGR aop_gpio   raw before floor @80010 reg=000000ff warm=0 hold=0
PMGR aop_cpu    raw before floor @80040 reg=0f0000ff warm=0 hold=0
PMGR aop_filter raw before floor @80048 reg=0f0000ff warm=0 hold=0
PMGR aop_busif  raw before floor @80050 reg=0f0000ff warm=0 hold=0
```

`0x0f0000ff` = target `0xf` / actual `0xf`, min-state 0, **AUTO clear**.
The `0x1f0000ff` + `auto=1` in `capture-active-cold-1.log` is written later by
`apple_pmgr_ps_init_state()`: the cold path runs
`regmap_update_bits(… APPLE_PMGR_AUTO_ENABLE)` for every domain that is already
active (`patches/boot/kernel/0007-…`, [E]). On warm boots `ps->keep_on` is true,
`power_on()` clears AUTO and never re-sets it, hence `0x0f0000ff` there too.
**The domain delta is our own driver, not iBoot.**

### 1.2 Nothing in the chain can start the AOP

> Private graphics-dependent excerpt omitted from this source snapshot.

m1n1's ladder, for the record (`src/akf.c`, `src/akf_fw.c`, [E]):

```c
if (ADT_GETPROP(adt, fw_node, "pre-loaded", &pre_loaded) < 0) return false;   /* j71t: absent */
if (!pre_loaded) { printf("akf: IOP firmware not pre-loaded.\n"); return false; }
```

j71t's `aop` node and `iop-aop-nub` (static ADT `docs/adt/j71t.txt:1856-1889`,
live plane `build/firmware/private/IODeviceTree.txt:907-917`) carry
`firmware-name`, `region-base`, `region-size`, `aop-target`,
`watchdog-enable`, `coredump-enable` — and **no** `pre-loaded`, **no**
`running`, **no** `no-firmware-service`.

### 1.3 Why the survivor reading is the only one left

The AOP mailbox is *enabled and empty* (`0x00020001` on both inbox and outbox).
Enabling it is an explicit AP write in both implementations:
`AppleA7IOPV1_setOutboxEnable` (`fffffff005b4e5d8`, called from `start`'s
`+0x678(1)`) and our own `aop_wr(AOP_OUTBOX_CONTROL, control | 1)`. A fresh
core that had just booted with the FIFO disabled would either still hold its
HELLO word or have had it dropped; the observed combination
*running + mailbox enabled + FIFO empty + no pending HELLO* is what an
**already-handshaked** firmware looks like. A reset-default reading is possible
only if the outbox enable bit powers up as 1, which would make both the kext
and the driver's writes pointless [I].

The same evidence says the firmware was never *stopped*: iOS's `stop`
(`fffffff005b4b8fc`) calls `+0x678(0)` (clear outbox enable) and `setCPURun(0)`
before gating the domains, and none of those three writes is visible. So the
iOS boot that owned the AOP ended without running the AOP driver's stop path —
a panic, a force reset, or a power-button shutdown at a point where the AOP's
PM tree had already been marked unmanageable. That is precisely how a surviving
instance looks, and it also explains why no HELLO is pending: the handshake was
consumed by that AP.

The remaining candidates and why they lose:

* **iBoot on this boot** — it does not execute on the pongoOS payload chain,
  and even on a normal iOS boot the 20H350 manifest marks
  `Firmware/AOP/aopfw-ipad6faop.im4p` with `IsLoadedByiBoot = false`,
  `IsFUDFirmware = true` ([E], `docs/periph/ds-aop-accel.md` §3). iBoot has no
  reason to touch the AOP, and no AOP code path is known in it.
* **pongoOS / m1n1** — see §1.2.
* **A previous Linux boot** — no driver ever wrote; the active driver refused
  on its first run and the observer is read-only.

So the instance predates the chain. It was created by an AP-side iOS boot:
`AppleSPUFirmwareService` requests the FUD image, `RTBuddyFirmware_loadAndPatch`
copies it into `0x210e00000` and patches the tag table, `AppleA7IOP::start`
powers the domains and releases the CPU. Nothing since then has gated the AON
island or asserted the AOP reset, so the same core, image and mailbox state are
still there — including across DFU entries, which reset the AP but not the
always-on rail (`aot-power = 1` in the live ADT).

The 20H350 IPSW contains exactly one AOP image pair
(`aopfw-ipad6faop.im4p`, build string
`ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465`), and this iPad has only
ever run the iOS that shipped with it, so whatever loaded the AOP used that
build. The one path that could put iBoot in front of the AOP is Hoolock's
separate *iBoot method* (`third_party/hoolock-docs/tutorials/SETUP_iBoot.md`);
`scripts/boot/autoboot.sh` does not use it. [E] for the manifest/tree, [I] for
"that build is what runs".

### 1.4 The one loose end (and the two reads that close it)

`cpu_control (+0x44) = 0` while `+0xb88 bit 0 = 1`. Both iOS's `start`
(`setCPURun(1)`, `fffffff005b4e7c8`) and its `stop` (`setCPURun(0)`) write that
bit, so a clean iOS start should have left it set. Two readings remain [U]:

1. the bit is a self-clearing/level-insensitive release on the AOP and the core
   keeps executing (so iOS could have started it);
2. the AOP was released through `auto-boot` only (`0x210000600 = 1`) by a loader
   that does not touch `+0x44`, and `+0x44` was never set.

Both reads are one MMIO word and neither is in the current snapshot:

| Read (read-only) | Offset | Interpretation |
| --- | --- | --- |
| `auto-boot` | `0x210000600` | `1` → a documented AP release happened and was never cleared; `0` → the release came from something that does not use the register |
| region tag table | `0x210e00000 + 0x389b8`, 0x1df B | patched (`AOPt=8`, `WrAd=0x210800000`, `nCal=0`, `SOC_=0x8003`) → a full AP loader ran; pristine (`AOPt=0`, `WrAd=0`, `nCal=1`, `SOC_=0xffffffff`) → no RTBuddy/AFK loader ever touched it |

§3.2 turns both into preflight checks anyway, so they cost nothing.

### 1.5 What a true power cycle would show, and why that is not a way out today

Battery disconnect (or a PMGR gate, which the driver must not do) drops the AON
island; the next boot would show target/actual `0`, `cpu_running = 0`, empty
FIFOs, and the active driver would take its cold path… and stop at
`refused-domain-state`, because `apple_pmgr_observe()` requires all five domains
on and the driver never forces one on. A post-power-cycle run therefore needs a
legitimate DT power handle (a `power-domains` link, or an `apple,always-on`
baseline) — a DTS/driver change for a later round, not an attach problem. [I]

---

## 2. Q2 — what iOS does with an IOP it finds already running

### 2.1 `AppleA7IOP`: one flag, one error, no status read

`AppleA7IOP::startCPUWithOptions` (`fffffff005b4b738`, vtable slot `+0x548` of
the abstract AppleA7IOP table at `fffffff006d3e158`) [E]:

```text
ldr x8,[x0,#0xb0]; cbz -> assert            ; firmware object required
ldrb w8,[x19,#0x110]; cbz ...               ; _started flag
  -> log "AppleA7IOP(%s):WARNING: CPU already running"
     (cstring fffffff00530ec1b), return 0xe00002d5   ; kIOReturnBusy
```

then, in order: `+0x620` (prepare, non-cold only), `setActive(1)`
(`fffffff005b4b6f4` → `callPlatformFunction("setActive", 1)` on `[this+0x128]`),
map firmware (`fw->vt[+0x540]` → `+0x6a0` = `setFirmwareRemap`
`fffffff005b4e72c`), `_started = 1`, the two IRQ sources
(`[this+0x160]->vt[+0xb0]`, `[this+0xe8]->vt[+0xb8]`),
outbox enable (`+0x678(1)` = `fffffff005b4e5d8`), optional timebase sync
(`+0x6c0`, only with `force-timebase-sync`), and finally `setCPURun(1)`
(`+0x6b0` = `fffffff005b4e7c8`: `+0x44 = (+0x44 & ~0x10) | 0x10`
and `*(0x210000600) = 1` when the `auto-boot` property mapped `[this+0x108]`).
`+0x658`, the gate in front of that block, is a stub that returns 1
(`fffffff005b4c654`).

It **never reads `+0xb88`** to decide anything; the only "already running"
notion is its own boolean. `stop` (`fffffff005b4b8fc`) mirrors it and calls
`setCPURun(0)` — i.e. a clean iOS stop clears both `+0x44` bit 4 and
`auto-boot`.

Property mapping in probe (`fffffff005b4b238`–`fffffff005b4b48c`, [E]) is worth
knowing because it contains the only supported way to suppress the release:
`auto-boot` → reg[2] (`0x210000600`); `cpu-ctrl-filtered` clears `[+0x122]` so
`setCPURun()` becomes a no-op; `ignore-mailboxes` clears `[+0x112]`;
`ignore-gating` sets `[+0x14c]`.

### 2.2 `RTBuddy`: a DT flag, a hibernation path, and a panic

`RTBuddy::parseBootProperties` (`fffffff005b68764`) reads the *nub* properties
([E]):

| Property | Field | Meaning |
| --- | --- | --- |
| `pre-loaded` | `+0x2228` | DT says the image is already in the target region. Where it is consumed is not fully traced in this pass [U]: the copy in `loadAndPatch` is gated by separate firmware-object flags (`fw+0xb8`, `fw+0x98`) |
| `running` | `+0x2229` | IOP is already running: skip the tag patch, the system-tag pass, the version publish |
| `no-firmware-service` | `+0x222a` | defaults to true when `running` |
| `quiesced` | `+0xd4`/`+0x140` = 3 | AP power handling deferred; enables the warm boot-complete message |
| `cold-boot-after-hibernate` | `+0x11d` | force the cold path even after a hibernation restore |
| `sleep-on-hibernate`, `no-hibernate-sleep` | `+0x12b`/`+0x11c` | hibernation power policy |
| `stand-alone`, `no-shutdown`, `watchdog-enable`, `power-managed`, `user-power-managed`, `dont-power-on`, `reconfig-firmware`, `continuous-time`, `asc-dram-mask` | various | as in the kext |

`RTBuddyFirmware::loadAndPatch` (`fffffff005b6f8a4`) with `running` set [E]:

```text
providerPowerOnForLoad()
if (svc) svc->vt[+0x550](../release/OMISSIONS.md)
if (!fw->[0xb8] && !fw->[0x98]) copyImageToTarget(fw, rtb)     ; b6f340
build memory map
if (rtbuddy->[0x2229] != 0) goto writeBackPatchBay              ; <-- running
svc->vt[+0x558](../release/OMISSIONS.md)        ; AppleSPU DTPatchbayTable (b88930)
fw->vt[+0x568](../release/OMISSIONS.md)    ; system tags (b70290) incl. AOPt/EC0p/nCal
fw->vt[+0x570](../release/OMISSIONS.md)         ; extra windows (none on j71t)
publishVersionProperties()
writeBackPatchBay()        ; b6f9b4
```

So `running` skips the tag writes and version properties but still rewrites the
TLV region with the bytes it just copied out — harmless, but it *is* a write.

The state machine (`RTBuddy::powerStateTransition`, `fffffff005b65b54`) is where
"already running" becomes interesting [E]:

```text
case 1 (boot):
  if (rtbuddy->[0x117] == 0) { loadAndPatch(); rtb->[0x117] = 1; }
  rc = provider->vt[+0x548](../release/OMISSIONS.md)     ; startCPUWithOptions
  if (rc == 0xe00002d5)                 ; kIOReturnBusy
        FUN_fffffff005b7d7dc(rtbuddy);  ; call site fffffff005b66000
        -> _panic("\"RTBuddy(%s)::%s \" \"already running (current state: %d)\" ...")
  setState(4); if (oldStatus == 3) send 0x0060000000000220
  rc = waitForBootState();              ; state 8, or 0x8000, timeout = "timeout" prop (default 0x14 s)
```

`FUN_fffffff005b7d7dc` is a panicking assert, not a log line: iOS treats
"provider says busy" as a programming error. There is no fallback path, no
retry, no protocol message.

The one place iOS genuinely attaches to an IOP that stayed alive is
**hibernation** [E]:

```text
FUN_fffffff005b649f0 (fffffff005b649f0)  reads IOHibernateState from the nub or PMRootDomain
hibernation branch (fffffff005b64bd0..fffffff005b64c14):
  if (hibernateState == 1 && (cold-boot-after-hibernate || no-hibernate-sleep))
        provider->vt[+0x5b0]();        ; powerOnForLoad, no firmware image
        FUN_fffffff005b64d00(rtb, 4, 4) ; pretend the boot handshake already happened
cold-boot-after-hibernate (fffffff005b651e0):
  force rtb->[0x140] = old status, rtb->[0xd0] = 3  ; cold path
```

Why this does not help us: the hibernation path assumes the **AP-side objects**
(RTBuddy instance, endpoint objects, endpoint numbers, ring pointers, service
bindings) are restored from the hibernation image, and that the IOP's own
endpoints still reference those same buffers. We have neither — a kexec'd Linux
kernel starts with empty objects and reuses all of DRAM. iOS never re-derives
an endpoint map from a running firmware; it cannot, because there is no
management message that asks for one.

### 2.3 The complete management vocabulary (nothing else exists)

RX (IOP→AP), dispatcher `fffffff005b71018` [E]:

| Type | Word (payload bits 55:52) | Handler |
| --- | --- | --- |
| 1 HELLO | `0x0010 \| max<<16 \| min` | `fffffff005b7196c`: accepts when `min < 0xd && max > 0xb`, sets state 5, replies `0x00200000000c000c \| (debugger \| tracing<<1)<<32`; otherwise replies `0x…ff0bad`, state `0x8000`, panics "Incompatible protocol version" |
| 4 PING_ACK | `0x0040 \| … \| seq` | compares against `rtbuddy+0xf8` (initial **0x16**) and bumps it |
| 7 IOP power | `0x0070 \| … \| state` | low byte `0x20` → state 8 (boot complete) |
| 8 EPMAP | `0x0080 \| last<<51 \| base<<32 \| bitmap` | `fffffff005b71afc`: creates endpoints, replies, may send AP power |
| 0xb AP power ack | `0x00b0 \| ack` | stores the ack, wakes the waiter |

TX (AP→IOP), all through `rtbuddy->vt[+0xe8]` (`RTBuddy::sendMessage`,
`fffffff005b66aa8`) [E]:

| Type | Word | Sender |
| --- | --- | --- |
| 2 HELLO_REPLY | `0x00200000000c000c` (iOS adds flags in bits 32+) | `fffffff005b7196c` |
| 3 PING | `0x0030 \| (uptime&0xffff)<<32 \| seq` | `fffffff005b71f1c` (seq at `+0xf4`, initial **0x15**) |
| 5 STARTEP | `0x0050 \| ep<<32 \| 2` | builder `fffffff005b717a4`, only when state ≥ 4 |
| 8 EPMAP reply | `0x0080 \| last<<51 \| base<<32 \| (base==0)` | `fffffff005b71afc` |
| 0xa AP power | `0x00a0 \| on` | `fffffff005b716d0`, deferred while `provider+0x140 < 6` |
| 6 boot-complete request | `0x0060000000000220` | `fffffff005b71fc4`, only when the old IOP status was 3 (`quiesced`) |

There is no "wake", "resume", "attach", "reset queues", "get endpoint map" or
"re-handshake" message. That is the whole protocol.

### 2.4 Answer in one sentence

iOS either (a) is told by the bootloader's DT that the IOP is `running` and
skips the patch/version work while still going through the same boot state
machine, (b) restores its own AP-side state from a hibernation image and jumps
straight to state 4, or (c) **panics** if the provider answers `kIOReturnBusy`.
It never sends an AP-initiated wake and it never reloads over a core it did not
start itself.

---

## 3. Q3 — a safe attach for our driver

### 3.1 What the attach path may and may not do

Inherited invariants (driver header, round-3 docs): never reset the AOP, never
force a power domain, never clear a mailbox fault, never write ring headers,
never retry with different parameters on the same boot.

Additional invariants this design adds, all derived from §2:

* No write to `+0x44`, `0x210000600`, the remap block, the interrupt mask
  registers, the region, or the tag table. A running firmware owns all of them.
* No STARTEP before an EPMAP that *we* received and acknowledged.
* No doorbell (`0xa2`) and no AFK traffic until an endpoint has answered `0x80`.
* Every TX is one word per step, with a hard reply deadline; the first
  unexpected word ends the attempt.

### 3.2 Phase A — read-only preflight (no mailbox traffic)

Run before anything is sent. All reads are MEM_TYPE_IO reads of the wrapper or
of the firmware region; neither has read side effects.

**A1. Wrapper snapshot** (already implemented): domains on, `+0x4008`/`+0x4020`
fault bits 18/19 clear, `+0xb88` bit 0 set, `+0x4020` bit 17 (empty) set.
New: log `+0x44` and `auto-boot` (`0x210000600`) — two words, §1.4.

**A2. Region fingerprint** at `region + 0`, read-only, ~0x4c0 bytes total:

| Window | Offset | Expected for `ipad6faop-20H350.bin` (342336 B, sha256 `af5d51a3…`) |
| --- | --- | --- |
| image head | `+0x0000`, 0x40 B | equal to the file head |
| `uuid` header | `+0x0020`, 0x40 B | `75 75 69 64 05 00 00 00`, TLV off `0x000389b8` at `+0x48`, TLV len `0x000001df` at `+0x4c`, word `0x0003a000` at `+0x50` (meaning not verified) |
| build string | `+0x47440`, 0x40 B | `ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465` |
| `AOP Sensors` | `+0x39298`, 0x20 B | optional sanity |
| tag table | `+0x389b8`, 0x1df B | see A3 |

**A3. Tag table** (little-endian 4-byte tags, `[tag][u32 len][data]`), with the
file's *unpatched* defaults and the values a loader must write:

| Tag | Len | File default | Loaded value |
| --- | --- | --- | --- |
| `AOPt` | 4 | 0 | 8 (`aop-target`) |
| `EC0p` | 8 | 0 | AP PA of the ext segment, page aligned |
| `ED0p` | 8 | 0 | 0 (no ED0 segment on j71t) |
| `WrAd` | 8 | 0 | `0x210800000` |
| `McRA` | 4 | `0x4000` | `0x4000` |
| `nCal` | 1 | 1 | 0 |
| `RTTO` | 8 | 0 | 0 |
| `SOC_` | 4 | `0xffffffff` | `0x8003` |
| `SOCR` | 4 | `0xffffffff` | 1 |
| `STKG` | 8 | `0x00000aff` | random, one zero byte |
| `PRNG` | 32 | 0 | 32 random bytes |
| `NONC` | 8 | 0 | random (iOS; Linux leaves 0) |
| `ECID` | 8 | 0 | chip ECID (iOS; Linux leaves 0) |

**A4. Ext segment** (`EC0p`, only when the table is patched). Read 0x90a0 bytes
at that PA and compare with `ipad6faop_ext-20H350.bin` (37024 B, sha256
`2528d268…`). The comparison answers "is the firmware's `0x0106e000` view still
the image it loaded", but it does **not** answer "will it stay that way": the
buffer was allocated by the previous AP with `rtbuddy->vt[+0x558]` (ordinary
DRAM, no DART on j71t) and freed when that AP died, so Linux now owns the pages.
Three outcomes:

| `EC0p` location | Contents | Meaning |
| --- | --- | --- |
| inside a `/reserved-memory` region (or the AOP region) | match | safe: the mapping cannot be recycled |
| anywhere else | match | intact **today**, but any later allocation can overwrite it: unsafe for a long-lived driver unless the range is reserved first (`memblock_reserve` while it is still free, or a reserved-memory hand-off through the FDT merge) |
| anywhere | mismatch | the firmware is executing against pages Linux already reused: `refused-running-stale-ext`, and the AOP should be considered damaged until the next power cycle |

This is the hard limit of the whole idea: the tag table is the only record of
where the ext buffer is, and we may not rewrite it while the firmware runs.

Decision table:

| A1 mailbox | A2 identity | A3 tags | A4 ext | Action |
| --- | --- | --- | --- | --- |
| fault bits set | any | any | any | `refused-mailbox-fault` (unchanged) |
| `+0xb88` bit0 clear | — | — | — | existing cold path (needs domains on) |
| running, FIFO not empty | any | any | any | existing transcript path (must start with HELLO) |
| running, FIFO empty | mismatch | any | any | **`refused-running-foreign-image`**, no TX |
| running, FIFO empty | match | pristine (no loader) | — | **`refused-running-no-loader-tags`**, no TX: a running core with an unpatched table is not something we understand |
| running, FIFO empty | match | patched, inconsistent (`WrAd`/`McRA`/`SOC_`) | — | **`refused-running-tag-mismatch`**, no TX |
| running, FIFO empty | match | patched, consistent | stale/unreadable/mismatch | **`refused-running-stale-ext`**, no TX |
| running, FIFO empty | match | patched, consistent | intact, reserved | continue to Phase B |
| running, FIFO empty | match | patched, consistent | intact, not reserved | **`refused-running-unreserved-ext`**, no TX, unless `attach=1` reserves the range before any allocation uses it |

### 3.3 Phase B — ping (the only stateless AP→IOP message)

Send exactly one ping and wait 300 ms; up to 3 attempts, 1 s total.

```text
T ep0  0x0030_0000_<uptime16>_<seq32>      seq starts at 1, uptime = boottime seconds
R ep0  0x0040_...._<ack32>                 ack32 == seq  or  seq+1
```

**The `seq+1` tolerance is mandatory.** iOS sends `rtbuddy+0xf4` (initial
`0x15`) and accepts `rtbuddy+0xf8` (initial `0x16`), bumping both by one per
exchange (`fffffff005b71f1c`, `fffffff005b71018` case 4, [E]). Our driver today
compares `(u32)payload == aop->ping_seq` (`apple-a9-aop-active.c`, `A9_MGMT_PING_ACK`)
and would therefore reject every real ack and hit `failed-ping-timeout`.

Outcomes:

| Observation | Meaning | Next |
| --- | --- | --- |
| type 4, `ack == seq` or `seq+1` (twice) | firmware alive, protocol 12, mailbox bidirectional | Phase C |
| type 4 with any other value, or any non-type-4 word | foreign protocol or a confused firmware | `refused-running-unexpected-mgmt`, stop |
| nothing in 1 s | no protocol behind `+0xb88` bit 0 (halted core, foreign firmware, or a dead mailbox) | `refused-running-no-ping-ack`, stop |

A ping is the least invasive message in the protocol (the firmware's own
watchdog exchange), carries no address, and cannot trigger DMA. It is also the
only way to distinguish "core out of reset" from "firmware executing protocol
12" — which is the one assumption the current refusal makes without evidence.

### 3.4 Phase C — HELLO_REPLY, and the only thing that can make attach work

If (and only if) Phases A and B pass, send the fixed reply once:

```text
T ep0  0x0020_0000_000c_000c
```

This is the exact constant iOS builds in `handleHello` (`fffffff005b7196c`)
with the debugger/tracing bits 32+ zeroed. The firmware's management state
machine is expected — [I], the firmware side is not in any artifact we have —
to be sitting at "HELLO sent, waiting for the AP's reply"; receiving it moves it
to the EPMAP state, exactly as on a cold boot. Whether the firmware re-sends
HELLO on a timeout, or accepts a duplicate reply, is [U].

Then wait 2 s for EPMAP (type 8). Outcomes:

| Observation | Meaning | Next |
| --- | --- | --- |
| type 8 EPMAP chunk(s) | the firmware had **not** completed a handshake with anyone: no endpoints, no rings, no DMA. We are its first AP. | adopt: continue with the cold-path bring-up (§3.5) |
| anything else, or silence | a previous AP already owns the firmware; its endpoint map is in objects we do not have and its rings may point into memory Linux now uses | `refused-running-handshake-owned`, stop, no retry |

Risk note, stated plainly: a duplicate HELLO_REPLY delivered to a firmware that
has already handshaked is unmodelled ([U]). The justification for allowing it
once is that the message is a fixed 56-bit constant with no addresses, the
management endpoint is a dispatcher (not a memory operation), the whole attempt
is one word with a 2 s bound, and the alternative — never asking — leaves the
accelerometer permanently unavailable on every boot of this device. It stays
opt-in (`apple_a9_aop.attach=1`, default 0, implies `active=1`) and off in any
configuration used for unattended boots.

### 3.5 After EPMAP: exactly the cold path, minus the load

No difference from round 3's flow, because the firmware is in the same state as
just after a cold load:

1. reply EPMAP per chunk (`low32 = base == 0`, last bit mirrored), then AP power
   `0x00a0_0000_0000_0001` [I];
2. ping loop (1 s cadence, 5 s timeout) — with the `seq+1` fix;
3. wait ≤2 s for IOP power state `0x20`; log if absent;
4. STARTEP `0x0050_0000_0000_0002 | ep<<32` for every endpoint ≥ 0x20;
5. per endpoint: `0x80`/`0xa0`, `0x89` → `0xa1|pa`, `0x8a`/`0x8b`/`0x8c` → ring
   header check (AOP-owned), `0xa3`/`0x86`, `0x85` → RX;
6. ready report `0xc0` "accel" service `0x10000007` → bind channel;
7. **factory calibration** (per-device file, keys in iOS order) then batch
   `0x16`, raw `0x5c`, report `0x00`;
8. first `0xc2` → IIO.

Calibration and endpoint setup are *required*, not optional: a firmware that
never handshaked has never seen a calibration table, has no endpoints and no
rings. The only thing it already has is the image, the tag table and the ext
segment (verified in A2–A4).

### 3.6 Bounded budget

| Step | Bound | On expiry |
| --- | --- | --- |
| A1–A4 | plain reads, < 1 ms | refuse with the matching string |
| B ping | 3 × 300 ms | `refused-running-no-ping-ack` |
| C HELLO_REPLY → EPMAP | 2 s | `refused-running-handshake-owned` |
| EPMAP replies | 2 s | `failed-no-epmap` (existing) |
| IOP power 0x20 | 2 s | warn and continue (existing) |
| service bind | 5 s | `failed-no-accel-service` (existing) |
| command replies | 2 s each | `failed-configure` (existing) |

Total worst case before a decision: ~3 s, all of it after Phase A has already
proved the region holds our image.

### 3.7 Optional diagnostics (default off)

* **Boot-complete request** `0x0060000000000220` → expect type 7 with low byte
  `0x20` within 2 s. iOS sends this only when the DT said `quiesced`
  (`fffffff005b71fc4`, caller `fffffff005b65b54`), so its effect on a firmware
  that was never quiesced is [U]. Useful to learn whether the running firmware
  is in the "AP complete" state; not part of the attach.
* **`auto-boot` + region dump**: expose `0x210000600`, `+0x44`, and the A2/A3
  windows through the existing debugfs `state`/`patchbay` files so the operator
  can settle §1.4 without a second driver build.

### 3.8 The zombie case must be loud

When Phase C fails (`handshake-owned`), the firmware is still running with an
endpoint map and possibly rings from an earlier AP. Linux has since reused that
DRAM. The driver cannot fix this and must not try; it should:

* log one `dev_warn` naming the situation and the evidence (ping ok, no EPMAP);
* keep the device in `PHASE_REFUSED` with a distinct result string, and say in
  the message that the next boot must come from a real power cycle, not kexec;
* still refuse to kexec (`shutdown` must not claim the AOP is stopped).

### 3.9 Driver changes this implies (specification only, not implemented here)

1. Split `refused-running-no-pending-hello` into the Phase A/B/C results above.
2. Add the `seq+1` ping tolerance and require two consecutive acks.
3. Add the read-only region fingerprint + tag/ext validation (`A2`–`A4`) before
   any mailbox write, gated by `apple_a9_aop.debug=1` for the dump.
4. Add `auto-boot` and `+0x44` to the `state` snapshot.
5. Add the opt-in `apple_a9_aop.attach=1` gate for Phase C; without it, the
   driver keeps today's refusal (Phase A/B still run and are reported).
6. Never take Phase C after a failed Phase B, and never retry either on the
   same boot.

---

## 4. Next measurements (read-only, no driver change needed)

1. `0x210000600` and `0x210800044` from the live kernel (devmem/observer range).
2. `0x210e00000 + 0x20..0x60`, `+0x389b8..+0x38b97`, `+0x47440..0x47480`:
   the identity and loader fingerprint.
3. `EC0p` from the tag table, then that PA's first 0x90a0 bytes vs
   `ipad6faop_ext-20H350.bin`.
4. Optional: one ping via a debugfs write (single word, 300 ms) to learn
   whether the running firmware answers protocol 12 at all.

Interpretation is already fixed by §3.2 and §3.3; none of these writes anything.

---

## Appendix A — message words used here

```text
HELLO (device)        0x0010 | max<<16 | min
HELLO_REPLY (AP)      0x00200000000c000c            (+flags<<32 on iOS)
PING (AP)             0x0030 | (uptime&0xffff)<<32 | seq
PING_ACK (device)     0x0040 | ... | ack             ack == seq or seq+1
STARTEP (AP)          0x0050 | ep<<32 | 2
EPMAP (device)        0x0080 | last<<51 | base<<32 | bitmap
EPMAP reply (AP)      0x0080 | last<<51 | base<<32 | (base == 0)
AP power (AP)         0x00a0 | on
AP power ack (device) 0x00b0 | ack
boot-complete req (AP)0x0060000000000220            quiesced path only
IOP power (device)    0x0070 | ... | state           state & 0xff == 0x20
```

## Appendix B — addresses cited

| Address | What |
| --- | --- |
| `fffffff005b4b738` | `AppleA7IOP::startCPUWithOptions` (busy return at `…b794`, string `…530ec1b`) |
| `fffffff005b4b8fc` | `AppleA7IOP::stop` |
| `fffffff005b4b6f4` | `setActive` → `callPlatformFunction("setActive")` |
| `fffffff005b4e72c` | `setFirmwareRemap` (V1 `+0x6a0`) |
| `fffffff005b4e7c8` | `setCPURun` (V1 `+0x6b0`): `+0x44` bit 4, `auto-boot` |
| `fffffff005b4e850` | timebase sync (V1 `+0x6c0`, `force-timebase-sync` only) |
| `fffffff005b4e4b8` / `fffffff005b4f6c4` | `isCPURunning`: `+0xb88` bit 0 (V1 / V2) |
| `fffffff005b4e624` | register-status dump (`0x4040/0x4000/0x4004/0x4008/0x4020`) |
| `fffffff005b4b238`–`…b48c` | probe property mapping (`auto-boot`, `cpu-ctrl-filtered`, …) |
| `fffffff006d3e158` | abstract AppleA7IOP vtable (`+0x548` start, `+0x6c8` setActive) |
| `fffffff006d3ff98` | AppleA7IOPV1 vtable (`+0x6a0/+0x6b0/+0x6c0`) |
| `fffffff005b68764` | `RTBuddy::parseBootProperties` |
| `fffffff005b6f8a4` | `RTBuddyFirmware::loadAndPatch` (`running` branch at `…b6f928`) |
| `fffffff005b65b54` | `RTBuddy::powerStateTransition` (panic call site `…b66000`) |
| `fffffff005b7d7dc` | `"already running (current state: %d)"` panic helper |
| `fffffff005b66630` / `…b666e8` | `waitForBootState` / gated (state 8 or 0x8000) |
| `fffffff005b71018` | management dispatch (`+0xf4 = 0x15`, `+0xf8 = 0x16`) |
| `fffffff005b7196c` | `handleHello` (`0x00200000000c000c`) |
| `fffffff005b71afc` | `handleEndpointMap` |
| `fffffff005b716d0` / `…b71f1c` | AP power / ping senders |
| `fffffff005b717a4` | STARTEP builder |
| `fffffff005b71fc4` | boot-complete request `0x0060000000000220` |
| `fffffff005b71268` | ping watchdog timer |
| `fffffff005b649f0` | `getHibernateState` (`IOHibernateState`) |
| `fffffff005b64bd0`–`…b64c14` | hibernation attach (state 4/4 without a load) |
| `fffffff005b651e0` | `cold-boot-after-hibernate` forces the cold path |
| `fffffff005ba9754` / `…ba9808` | AFK start `0xa3` / stop `0xa4`/`0xc2` |
| `fffffff005ba8bb4` | GET_BUF reply `0xa1 \| pa` |
| `fffffff005baba00` / `…bab540` | ring reader / writer |

## Appendix C — firmware identity

```text
main  apple/aop/ipad6faop-20H350.bin      342336 B  af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8
ext   apple/aop/ipad6faop_ext-20H350.bin   37024 B  2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22
uuid header at +0x20 (v5), TLV at +0x389b8 len 0x1df, RTSZ tag 0x6e000 (the word at +0x50 reads 0x3a000)
build string at +0x47440: ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465
```
