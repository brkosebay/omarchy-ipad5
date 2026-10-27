# The running AOP image: what the region holds, and how iOS reads it

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

Legend: **[V]** read from an artifact (instruction bytes, decompiler output,
data in the shared Ghidra project, or a saved capture); **[V-dev]** verified on
this iPad by a capture already on disk; **[I]** inference; **[U]** unresolved.
Kernel addresses are unslid; add the boot slide (`p1ctl kslide`).

Kernelcache: `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`,
SHA-256 `c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
Pinned firmware: `ipad6faop-20H350.bin`, 0x53940 bytes, SHA-256
`af5d51a3…047c8`; ext `ipad6faop_ext-20H350.bin`, 0x90a0 bytes.

---

## 0. Bottom line

1. **The firmware iOS parsed for this boot is the pinned 20H350 image**
   (`FirmwareUUID = cce47592-d067-374e-8b46-151be730c1de`,
   `FirmwareVersion = ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465`,
   `RTKitVersion = RTKit-2062.142.1.release`). **[V-dev]**
   But those properties are read from the **image file held in kernel memory**,
   not from the region, so they identify the file, not the executing bytes.
   **[V]** §7.
2. **iBoot/LLB do not load the AOP firmware.** The 20H350 manifest says
   `IsLoadedByiBoot = false`; the decrypted `iBoot.j71t` and `LLB.j71t`
   contain no `aopfw`/`ipad6faop` string and no `AOPt`/`EC0p`/`WrAd` patchbay
   tag, and no code that copies an image. iBoot's only *code* reference to the
   AOP region base
   (`mov x0,#0x10e00000; movk x0,#2,lsl#32`) **reads eight u32 from it**
   (region+0x00 and region+0x10) and programs four MMIO blocks with
   `(word & 0xff0000) | 4`. It also carries the region pair `0x210e00000` +
   `0x9b000` as data, next to a `0x210e9b000` + `0x5000` entry that tiles the
   static DT's `0xa0000`. **[V]**
3. **On our real cold boot the AOP is already running although no Apple boot
   stage ran** (checkm8 → pongoOS → m1n1, capture
   `build/periph/aop-r4/capture-active-cold-1.log`: `cpu_running=0x00020001`,
   mailboxes empty, idle `0x79`). The instance therefore **survives AP resets**
   — it is an always-on block, and "which firmware runs" is decided by whoever
   last started it, not by the current boot. **[V-dev] + [I]**
4. **The region is real, stable and reads identically at two widths.** Correct
   aligned `readl32` (round 6) reproduced the round 4 `memcpy_fromio` bytes.
   So the observed content is not an access-width artifact. **[V-dev]**
5. **That content cannot be the pinned image, and nothing in the inspected
   loader or startup code writes it.** The image's build text at +0x47440 is
   not there; its uuid at +0x20 is not there; the vector table at +0 is not
   there. The loader copies the raw image to region offset 0 and only patches
   the TLV table at +0x389b8. The firmware's reset stub moves `VBAR` to +0x4e0
   and zeroes BSS `0x53900..0x6d368` — it never touches +0 or +0x47440.
   Two captures from different boots show the head **changing**
   (`0x00030000,0x00070000` → `0x00040000,0x00080000`; word 0x44 `4` → `3`),
   which no firmware-side writer explains. **[V] + [V-dev]**
6. **So the region is a staging area, not (or no longer) the executing
   image.** Two mechanisms fit every observation, and the new `aop-dump`
   distinguishes them: (a) the AOP's boot flow consumes the staged image into
   on-chip memory and the region is afterwards used as scratch/shared memory;
   or (b) the image still executes in place but its first page and parts of its
   writable data span have been rewritten by the firmware. **[I]**
   What is *not* available in either case is an identity check based on
   region offset 0; §7.1 gives the check that is valid and §8 the tool that
   measures it.
7. **There is no second region.** The j71t AOP has exactly two DT ranges:
   wrapper `0x210800000/0x1c000` and region `0x210e00000/0x9b000`, plus the
   auto-boot word `0x210000600`, the `0x210000508` pair and `aop-gpio`
   `0x2100f0000/0x4000`. The nub has no `segment-ranges`, `segment-names`,
   `sram-index`, `pre-loaded` or `running` property. If the firmware executes
   from on-chip memory, that memory is **not described in the device tree**.
   **[V]**
8. **iOS keeps a kernel mapping of the region, and it is reachable from a
   statically verified object path.** `RTBuddyFirmware_createMemoryMap` maps
   the target with `IOMemoryDescriptor::map()` → `createMappingInTask(
   kernel_task,…)` and stores the `IOMemoryMap` in the firmware object at
   `+0xa8 → +0x28[0]`. `aop-dump` walks kdebug's coprocessor list (static list
   head, node name `"AOP"`, `KDebugCoreID = 2`) to the live `RTBuddy`, then
   `+0x21e0 → +0xa0[0]` (segment: physical base/size, gated against the ADT) →
   `+0xa8 → +0x28[0]` → `IOMemoryMap +0x28/+0x30`, and dumps the region, the
   remap registers and the identity windows read-only. **[V]**

---

## 1. The contradiction, stated precisely

Rounds 4–7 established, on the device:

* the AOP CPU is running, idle, with both FIFOs empty and no faults, on both a
  warm kexec and a checkm8 cold boot (`docs/periph/aop-r6-device-debugfs.txt`);
* the wrapper remap reads `+08/+10 = 0x210e00000`, `+18/+20 = 0`,
  `+28 = 0x1ff000`, `+38 = 1`;
* the first 0x100 bytes at AP PA `0x210e00000` differ from the pinned image in
  214 of 256 bytes, and the 64-byte window at +0x47440 is high-entropy data
  rather than the build string.

Statics say: iOS's normal loader copies the raw main image to **region offset
0** with aligned `u32` writes (`RTBuddyFirmware_copyImageToTarget`,
`fffffff005b6f340`) and patches only the TLV table. If that copy is what the
CPU executes, offset 0, offset 0x20 and offset 0x47440 must still show the
image. They do not. The three candidate resolutions are examined in §3–§5.

---

## 2. What the driver does, exactly (recap used below)

| Step | Code | Consequence |
| --- | --- | --- |
| target selection | `RTBuddy_getFirmwareTargetMemory` `fffffff005b68cdc` | `nub+0x2180[0]` (from `region-base`/`region-size`) unless `nub+0x2169` (force-DRAM) is set; otherwise `IOBufferMemoryDescriptor::withOptions` |
| image copy | `copyImageToTarget` `fffffff005b6f340` | target size `max(image_len, RTSZ)`; `memcpy_to32(map_base, image, len)` at **offset 0**; whole-target cache flush; **no BSS clear** |
| tag patch | `writeBackPatchBay` `fffffff005b6f9b4` | 35-tag TLV at region+`0x389b8`, size `0x1df`, aligned `u32` writes |
| ext segment | `AppleSPUFirmwareService_stageExternalSegment` `fffffff005b88090` | separate page-aligned DRAM buffer; its **physical address** goes into tag `EC0p` |
| map creation | `createMemoryMap` `fffffff005b6f310` → `withSingleDescriptor` `fffffff005b5da7c` → init `fffffff005b5d968` | one `IOMemoryMap` per segment via `IOMemoryDescriptor::map(0x10001000)` |
| map storage | `fw+0xa8` → `+0x28` (OSArray of `IOMemoryMap`) | persistent for the boot |
| image release | `loadAndPatch` `fffffff005b6f8a4` tail | `fw+0x88` (the image `OSData`) is **released** after load |

`IOMemoryDescriptor::map` (`fffffff00777f1fc`) is
`createMappingInTask(this, kernel_task, 0, options|1, 0, length)`, so the
mapping is a **kernel virtual address** of the firmware target.
`IOMemoryMap` layout: `getVirtualAddress()` = `+0x28`, `getLength()` = `+0x30`
(`fffffff007780634`, `fffffff007780758`). **[V]**

---

## 3. Candidate A — iBoot/LLB load the AOP firmware: **refuted**

**[V]** The 20H350 `BuildManifest` entry `Firmware/AOP/aopfw-ipad6faop.im4p`
has `IsLoadedByiBoot = false`, `IsLoadedByiBootStage1 = false`, `IsFTAB =
false` (round 1, re-checked against the saved component).

**[V]** Byte/string inventory of the decrypted boot firmware
(`build/power/oc-r3/bootrefs/20H350__iPad6,11/iBoot.j71t.RELEASE.im4p.dec`,
`…/LLB.j71t.RELEASE.im4p.dec`):

| probe | iBoot | LLB |
| --- | --- | --- |
| `aopfw`, `ipad6faop` | 0 | 0 |
| `AOPt`, `EC0p`, `WrAd` (patchbay tags) | 0 | 0 |
| `iop-aop-nub`, `arm-io/aop` | 2 | 0 |
| `region-base`, `region-size` | 1 each | 0 |
| immediate `0x10e00000`/`0x210e00000` | 1 | 0 |
| *code* immediate `0x9b000` / `0xa0000` | 0 | 0 |
| `0x210e00000` + `0x9b000` as data | file 0x6c818/0x6c828 | file 0x3c648/0x3c658 |

Those four strings sit in iBoot's own string pool (file 0x58800:
`…nvme-scratch-virt-region\0arm-io/aop\0iop-aop-nub\0region-base\0
region-size\0PCIe config…`), i.e. they are the device-tree vocabulary iBoot
hands to XNU, not a firmware loader. The single AOP-region *code* reference is
inside the mode-0 branch of a three-way platform callback at `0x870019364`:

```text
8700193cc  mov  x0, #0x10e00000
8700193d0  movk x0, #2, lsl #32        ; x0 = 0x210e00000, the AOP region
8700193d4  bl   0x87001afd8
```

`0x87001afd8` treats that address as a **source**: it reads four u32 from
`x20 + i*4` (region+0x00..0x0f) and writes `(word & 0xff0000) | 4` to
`0x200220700 + i*0x40000`, then reads four more from region+0x10..0x1f and
writes `0x200220708 + i*0x40000`, both for `i = 0..3`, through the register
helper `0x87001b488` (`w0 = 7`, tail-call `0x87000801c`). That is an
eight-word configuration table consumed at boot and applied to four
PMGR-like blocks; it is not an image copy and it does not touch the region. **[V]**

**[V]** The boot chain also carries the AOP region as *data*, in both stages:
iBoot file 0x6c818/0x6c828 and LLB file 0x3c648/0x3c658 hold `0x210e00000` and
`0x9b000` in the same two adjacent records, and the record immediately before
holds `0x210e9b000` with `0x5000`. The two ranges tile
`[0x210e00000, 0x210ea0000)` — exactly the static DT's `0xa0000`. **[I]** This
is the most likely origin of the live `region-size = 0x9b000`: the boot chain
carves the 0xa0000 aperture into a 0x9b000 firmware target and a 0x5000
remainder (purpose unknown; 5 pages). That closes the round-1 `0x9b000` vs
`0xa0000` question, and it is another confirmation that the boot firmware
*describes* the region but never fills it.

**[I]** It is consistent with the boot chain *expecting* the AOP region to be
owned by a retained AOP and to hold the AOP's own table at offset 0 — which is
exactly the shape the Linux capture shows (`words0_15` = 16 small integers).

**Device check:** on the round-3 "true cold boot" path
(DFU → checkm8 → pongoOS → m1n1 → Linux) no Apple boot stage executes, and the
AOP was still running (`cpu_running=0x00020001`, idle `0x79`, empty FIFOs). A
boot chain that never ran cannot have started it. The AOP therefore persists
across AP resets, and the instance we measure may be arbitrarily old. **[V-dev] + [I]**

*Consequence:* the identity question is not "which image does this boot load"
but "which image did the surviving instance start from". That is what §8
measures.

---

## 4. Candidate B — the firmware relocates or reuses its load area

### 4.1 What the loader writes

**[V]** (rounds 5–7, re-read this round)

* `copyImageToTarget`: target = the descriptor at `fw+0x98` (obtained from
  `nub+0x2178`), mapping at offset 0,
  `memcpy_to32` of the *unmodified* image bytes. No header skip, no
  decompression, no relocation table.
* `writeBackPatchBay`: only the TLV table at +0x389b8 (479 bytes) is patched in
  the region; the ext **physical address** goes into `EC0p`.
* `loadAndPatch` then releases the image `OSData` (`fw+0x88 = 0`), so after
  attach the driver no longer holds the image bytes at all.

### 4.2 What the firmware's own startup does

**[V]** From the pinned image's reset stub (file 0x68..0x4a4, ARM) and Thumb
entry at 0x27390:

| Observation | Value | Effect on offset 0 / 0x47440 |
| --- | --- | --- |
| execution base | derived from `PC` (`sub r0, pc, #0x78`) | position-independent: runs from *any* base, including on-chip RAM |
| `VBAR` after startup | `0x010004e0` (`file+0x290`) | bytes 0x00..0x4df are dead after the stub |
| page tables | tables at VA `0x0103a000..0x0103b000`, mapping `VA 0x01001000..0x01039dec → P+0x1000` and `VA 0x0103a000..0x0106d368 → D` | identity for text; **data span may be backed elsewhere** via the u32 at `image+0x58` (zero in the file → `D = P+0x3a000`) |
| stacks | `SP = 0x01047000`, `0x01047400` | grow down, *below* the build text at `0x01047440` |
| BSS zero | `DSZS/DSZL` default → `0x01053900..0x0106d368` | does **not** cover +0 or +0x47440 |
| optional data relocation | `DSRL/DSVS/DSCL`, all zero in the file | inert unless patched |

So a device whose image is copied to region offset 0 and executed in place
would still show `18 00 00 ea` at +0 and the build text at +0x47440 unless
something writes them. The observed content is therefore either a **different
memory** or **rewritten memory**. **[V] + [I]**

### 4.3 What the device shows, and what changes between captures

**[V-dev]** Two captures from different boots, same driver family:

| offset | R4 (`active-20261023T170147Z`) | R6 (`aop-r6-device-debugfs.txt`) |
| --- | --- | --- |
| 0x00 | `00000300 00000700 00000500 00000300` | `00000400 00000800 00000500 00000300` |
| 0x20 | `00001b00` ×4 | `00001b00` ×4 |
| 0x40 | `02 04 02 06` | `02 03 02 06` |
| 0x47440 | `69 95 7e 1a 17 b7 97 1f …` | `21 95 7a 1a 17 b6 97 3f …` |
| 0x47474 (guard) | `(inside window)` | differs from `0x00000aff` |

Two facts matter. First, offset 0 holds a **small table**, not the vector area;
its first four words are `0x00040000, 0x00080000, 0x00050000, 0x00030000`
(i.e. 4/8/5/3 in the high half). Second, the values **move between boots**
(3→4, 7→8, 4→3), and the 0x47440 window changes with single-bit differences
scattered across otherwise identical bytes — the signature of a buffer that is
written occasionally and not maintained as image data.

**[I]** Note the shape: iBoot consumes **8 u32** from region+0x00..0x1f and
applies `(word & 0xff0000) | 4` to hardware. The first eight words of the
device's table are `0x00040000, 0x00080000, 0x00050000, 0x00030000, 0, 0, 0, 0`
— values in bits 23:16, the same shape iBoot masks. That is suggestive but not
proof: the only place in any artifact where a writer of `region+0` is visible
is iBoot's *read*.

### 4.4 The two remaining mechanisms, and how to tell them apart

* **(B1) Stage-and-run-elsewhere.** The AOP boot flow copies the staged image
  into on-chip memory and starts it there; the region then belongs to the
  firmware as scratch/shared memory. Consistent with: no DT aperture for that
  memory (§5), a position-independent stub that works from any base, a table at
  offset 0 owned by the firmware, and no second copy in the region.
* **(B2) Run-in-place with a rewritten prologue/data.** The firmware executes
  the region copy but has rewritten its own first page (dead after `VBAR`
  moved) and parts of its writable data span (e.g. via `D` backing or by
  reusing `.data` tail as heap).

Both predict "region ≠ image". They differ in **which** spans differ:

* B2 requires the **text span** `0x1000..0x389b8` (and the tail `0x38b98..
  0x3a000`) to still match the image byte-for-byte, because those pages are
  read-only code mapped identity and never rewritten.
* B1 predicts the text span differs too (the whole region is free/scratch), or
  at least that it carries no stable relationship to the image.

`aop-dump` + `scripts/periph/aop/compare-region.py` answer exactly this:
dump all 0x9b000 bytes and print per-span difference counts, with the TLV
window and the two documented mutable words excluded from the "code must match"
rule. §8–§9.

---

## 5. Candidate C — a second region where text lives: **none in the DT**

**[V]** `docs/adt/j71t.txt` (static) and
`build/firmware/private/IODeviceTree.txt` (live, lines 885–1022) agree on the
`aop` node's resources:

| `aop/reg` | SoC address / size | AP PA | live `IODeviceMemory` | role |
| --- | --- | --- | --- | --- |
| 0 | `0x10800000 / 0x1c000` | `0x210800000` | `8866758656 / 114688` | wrapper: remap, CPU, mailboxes |
| 1 | `0x10e00000 / 0x9b000` (static says `0xa0000`) | `0x210e00000` | `8873050112 / 634880` | firmware region / target memory |
| 2 | `0x10000600 / 4` | `0x210000600` | `8858371584 / 4` | auto-boot |
| 3 | `0x10000508 / 8` | `0x210000508` | `8858371336 / 8` | unresolved |

`aop-gpio` is a separate node (`0x2100f0000/0x4000`). The `iop-aop-nub` child
has `region-base = 0x210e00000`, `region-size = 0x9b000` (live) / `0xa0000`
(static ADT), `firmware-name = ipad6faop`, `aop-target = 8`,
`coredump-enable = 32`, and **no** `segment-ranges`, `segment-names`,
`sram-index`, `pre-loaded` or `running`.

**[V]** The `0x9b000` / `0xa0000` discrepancy is now explained (§3): iBoot
carries a `0x9b000` firmware target plus a `0x5000` remainder inside the
`0xa0000` aperture, and the live nub reports the patched `0x9b000`.

**[I]** If the firmware executes from internal memory (B1), that memory has no
AP aperture and cannot be read from either OS. That possibility cannot be
excluded from the DT alone — but it also cannot be exploited, so the region
remains the only measurable object.

---

## 6. The verified layout, and what remains unknown

### 6.0 The answer in one paragraph

**Verified:** the driver stages the pinned main image at AP PA
`0x210e00000 + 0`, patches the 35-tag TLV at `+0x389b8`, places the ext segment
in a separate DRAM buffer and records its physical address in tag `EC0p`, and
programs the wrapper so that IOP address 0 maps to `0x210e00000` (delta
`0x210e00000`, lower bound 0, upper bound `0x1ff000`, enable 1). That is the
*staging* layout, and it is complete.
**Not verified:** the physical address of the **executing** bytes. The region
no longer (or never did) hold the image's head, uuid or build text at the
offsets the loader wrote them, so the executing image is either a copy outside
the region (B1, §4.4) or the same copy with a rewritten prologue and data span
(B2). `aop-dump` decides between them; until it runs, no claim about the
running image's physical layout beyond the staging description is justified.

### 6.1 What is verified

| Object | Value | Evidence |
| --- | --- | --- |
| staging target (AP PA) | `0x210e00000`, size `0x9b000` | DT + live nub properties **[V]**; driver segment **[V]** |
| image placement | raw main image at target offset 0, `memcpy_to32` | `copyImageToTarget` **[V]** |
| tag table | target+`0x389b8`, declared `0x1df`, aligned `0x1e0` read | TLV header, `locatePatchBay` **[V]** |
| ext segment | separate DRAM buffer; PA in `EC0p`; `0x90a0` bytes | `stageExternalSegment` **[V]** |
| wrapper remap (running) | `+08/+10 = 0x210e00000`, `+18/+20 = 0`, `+28 = 0x1ff000`, `+30 = 0`, `+38 = 1` | R6 fingerprint **[V-dev]** |
| wrapper remap (cold loader) | same but `+28 = 0x9b000` | `AppleA7IOPV2_mapFirmware` **[V]** |
| firmware's own translation | IOP PA within `[lower, upper]` (inclusive) → `PA + delta` | `0x0103028c` **[V]** |
| region at runtime | not image head / not uuid / not build text; head changes per boot | R4/R6 captures **[V-dev]** |
| identity at attach | the driver parsed `uuid` at image+0x20 and the version strings and published them — **from the `OSData` byte buffer, not from the region** | §7 **[V] + [V-dev]** |

### 6.2 What is not verified

* The **physical address of the executing code**. This is the core unknown and
  neither the region nor the DT names it.
* Whether `+28 = 0x1ff000` is the hardware's own value (the firmware writes
  `+0x2c`/`+0x30` in its late controller init) or a readback artifact. Round 7
  proved the *software* translator uses it as an inclusive upper bound; the
  hardware comparator is still [U].
* Whether the AP aperture and the AOP's view of "region" are the same memory
  for the whole 0x9b000 extent (only offsets 0, 0x20, 0x47440 and two full
  0x100/0x40 windows have been sampled).

### 6.3 One correction to round 7 worth stating plainly

Round 7's "remap is an additive delta + page bounds" is right, and with the
observed words it means the firmware's software view covers IOP pages
`0..0x1ff` (2 MiB) translated by `+0x210e00000`. That is a description of the
**firmware's DMA/pointer translation**, not of the AP's read aperture. The AP's
resource is still 0x9b000 and the driver's segment is still 0x9b000. Nothing in
this round widens either.

---

## 7. Identity: what iOS publishes, and why it is not enough

**[V]** `RTBuddyFirmware_publishVersionProperties` (`fffffff005b6f6dc`) sets
three properties on the `RTBuddy` node:

| property | source |
| --- | --- |
| `FirmwareUUID` | `uuid_unparse_lower(fw+0xd0)` after `findUUIDHeader` |
| `RTKitVersion` | string at offset `u32@header+0x08` |
| `FirmwareVersion` | string at offset `u32@header+0x0c` |

**[V-dev]** On this iPad (`build/firmware/private/IOService.txt`, lines
1520–1541) they read exactly:

```text
FirmwareUUID    = "cce47592-d067-374e-8b46-151be730c1de"
FirmwareVersion = "ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465"
RTKitVersion    = "RTKit-2062.142.1.release"
```

**The catch.** `readRange32` (`fffffff005b6fb78`) resolves an address through
`FUN_fffffff005b6fb34`, which prefers the **image `OSData`** (`fw+0x88`, vtable
slot `+0xe0` = `OSData::getBytesNoCopy(offset,len)`) and only falls back to the
memory map (`fw+0xa8`) when the image object is gone. `loadAndPatch` releases
the image object *after* publishing. So:

* these three properties prove **which file iOS parsed and patched** — for this
  boot, the pinned 20H350 image; **[V]**
* they do **not** prove that the CPU executes those bytes, because the AOP may
  have been started by an earlier session (§3). **[V] + [I]**
* `findUUIDHeader` searches eight candidate offsets
  `{0x20, 0xc0, 0x204, 0xc00, 0x1020, 0x1204, 0x4020, 0x4204}`
  (table at `fffffff005311068`) for the word `"uuid"` with version 4/5 — so a
  *file* check is a 0x20 signature plus a version, not a magic-at-zero. **[V]**

### 7.1 A valid identity check

**[I], derived from the code above.** An identity check for a *running* AOP
must (a) read memory that is known to be the driver's own target, (b) compare a
span that the firmware cannot legitimately rewrite, and (c) treat the file
identity as necessary but not sufficient:

1. **Required, cheap, no kernel memory:** the three published properties above
   plus `role = AOP`, `firmware-name = ipad6faop`, `aop-target = 8` and
   `region-base/region-size` from the nub. A mismatch means "iOS is not even
   carrying the pinned image" → stop.
2. **Required, driver-target bytes:** dump all 0x9b000 bytes through
   `fw+0xa8 → +0x28[0]` (the mapping the driver itself created) and diff them
   against the pinned file:
   * **code span** `0x1000..0x389b8` **plus** `0x38b98..0x3a000` must be equal.
     These pages are read-only, identity-mapped and never written by the
     inspected code.
   * **expected differences** are allowed only in: the boot stub page
     `0x000..0x4e0` (dead after `VBAR` moved to `0x4e0`), the TLV window
     `0x389b8..0x38b97` (loader-patched), and the two known mutable words
     `+0x47474` (STKG guard) and `+0x47478` (cursor).
   * BSS `0x53900..0x6d368` must be zero except for anything the firmware
     allocated there.
3. **Required, driver target metadata:** the segment physical base/size must be
   `0x210e00000`/`0x9b000`. A different target (a `IOBufferMemoryDescriptor`
   allocation, i.e. `nub+0x2169 != 0`) means the boot did **not** use the ADT
   region — that is a different, and testable, layout.
4. **Required, wrapper:** `+08/+10 = 0x210e00000`, `+18/+20 = 0`, `+38 = 1`, and
   record `+28` (the live readback, `0x1ff000`) without treating it as an
   address.

If step 2 finds the code span different, the running image is **not**
identified by this region and no Linux attach may treat the region as its own.

`scripts/periph/aop/compare-region.py` implements step 2 and prints a per-span
verdict.

---

## 8. iOS ground truth: the verified object path, and `aop-dump`

### 8.1 The path (all hops statically verified in the shared kernelcache)

```text
g_kdebug_coproc_list  0xfffffff0078e2b30            (DAT in _kdebug_register_coproc
                                                     FUN_fffffff007578dcc)
  node+0x00  char[32] name       == "AOP"           (ADT role; KDebugCoreID=2)
  node+0x28  callback            != 0
  node+0x30  context             -> RTBuddy instance
  node+0x48  next                bounded to 32 nodes, kernel-object range only

RTBuddy (context)
  +0x30  provider cache        -> AppleA7IOPNub   (vtable 0xfffffff006d442d8)
         guarded by +0x38 vs *(u32 *)(*(u64 *)(rtb+0x10)+0x10)   [IOService::getProvider]
  +0x21e0 RTBuddyFirmware*      (RTBuddy_attachFirmware 0xfffffff005b6479c)
  +0x2180 OSArray<segment>      (region list)
  +0x2178 IOMemoryDescriptor*   (target)
  +0x2169 char                  (force-DRAM flag)

AppleA7IOPNub
  +0x88  provider               -> AppleA7IOPV2    (vtable 0xfffffff006d3ff98)

AppleA7IOPV2
  +0x100 mapped wrapper base    (AppleA7IOPV2_mapFirmware 0xfffffff005b4e72c)

RTBuddyFirmware
  +0x88  OSData* image          (released after load; usually 0 at attach time)
  +0x98  IOMemoryDescriptor*
  +0xa0  OSArray<segment>*
  +0xa8  RTBuddyMemoryMap*
            +0x28 OSArray<IOMemoryMap*>
                     [0]+0x28 = region kernel VA, [0]+0x30 = length

RTBuddyMemorySegment  +0x18 physical base, +0x30 length
OSArray               +0x14 count, +0x20 elements (8 B)
OSData                +0x0c length, +0x18 bytes
```

Every hop is gated in that order: the node name and callback, the nub class
word, the segment physical base/size against `--region-pa/--region-size`, the
segment/map array counts (they are built 1:1), the mapping alignment/length,
then the wrapper class word. A failed gate prints why and stops **before** the
next pointer is dereferenced.

### 8.2 What it reads

| group | content |
| --- | --- |
| properties (userspace IOKit, no kernel memory) | all properties of `IODeviceTree:/arm-io/aop`, `…/iop-aop-nub`, `…/accel`; and the identity keys of `AppleSPUFirmwareService`, `RTBuddy`, `RTBuddyService`, `AppleA7IOPV2`, `AppleA7IOPNub` |
| wrapper (MMIO, read-only) | remap `+0x08..+0x3c`, `+0x44`, `+0xb88`, `+0x4008`, `+0x4020`, `+0x4040` — **no** `0x4038` (FIFO consume), no `0x4000/0x4004` |
| region | `min(segment size, --limit)` bytes from the driver's own mapping, page-bounded reads |
| image | the `OSData` bytes when the driver still holds them (usually not at attach time) |
| verdict | head/uuid/build windows, byte-diff against the `OSData` when present |

Outputs: `aop-report.txt` (all of the above), `aop-region.bin`, and
`aop-loaded-image.bin` when the image object is still alive.

### 8.3 Safety model

* No `mach_vm_write`, no MMIO write, no IOKit external method, no kernel
  allocation, no NVRAM/syscfg access. The only `write()` calls are its own
  output files (`O_CREAT|O_EXCL`, 0600, chowned to `SUDO_UID`).
* No scan. Every address comes from a static global or a verified object field;
  each read is page-bounded (`kread_span`), so a misplaced span cannot walk into
  an unmapped neighbour page.
* Fail-closed: an unexpected class word, an implausible collection count, a
  segment that is not the ADT region, or a missing map refuses **without
  reading a region byte**. `--dry-plan` prints every hop and the planned reads
  and dumps nothing.
* If iOS used a DRAM allocation instead of the ADT region, the tool reports the
  observed target and refuses; an operator can accept it explicitly with
  `--region-pa/--region-size` on a later run.

---

## 9. Orchestrator test plan (not run by this round)

**Preconditions:** iPad 5 (`iPad6,11`, `j71t`) on iOS 16.7.10, palera1n
rootless, USB SSH via `scripts/ipad-ssh.sh`, `p1ctl` present, and a boot after
which the AOP is known to be running (the default). No Linux kernel running.

**1. Build and deploy.**

> Private graphics-dependent excerpt omitted from this source snapshot.


**2. Property-only pass (no kernel memory at all).**

> Private graphics-dependent excerpt omitted from this source snapshot.


Read: `FirmwareUUID`, `FirmwareVersion`, `RTKitVersion`, `role`,
`region-base`, `region-size`, then the `plan:` lines. If the plan stops at
"no kdebug node named AOP", RTBuddy has not registered on this boot — stop.

**3. The dump (fresh directory; `O_EXCL` refuses to overwrite).**

> Private graphics-dependent excerpt omitted from this source snapshot.


With `--dir` the report goes to `aop-report.txt`; the `tee` captures the
verification lines (stderr) so a refusal is visible without copying files.

**4. Compare on the host.**

```sh
python3 -B scripts/periph/aop/compare-region.py --region "$R/aop-region.bin" \
  | tee "$R/compare.txt"
```

**Decision table.**

| observation | reading | next |
| --- | --- | --- |
| segment `phys=0x210e00000 size=0x9b000`, region head = the R4/R6 16-word table, **code span matches** | B2: the image executes in place; only the dead prologue page and writable data were rewritten | identity = pinned image; keep the region reserved, never write offset 0; the Linux cold loader must reproduce the same prologue page + TLV patch |
| same target, **code span differs** | B1: the staged image is not what runs | stop treating the region as identity; the only path to a Linux-owned AOP is a real AON cycle with a fresh load (still unsolved) |
| segment `phys` is a DRAM address (not `0x210e00000`) | iOS took the `IOBufferMemoryDescriptor` path this boot | the ADT region is not the target at all; record the PA and re-run with `--region-pa` to dump it |
| `fw+0xa8` null / "no kernel mapping" | iOS attached without creating a map (or did not attach) | re-run after a fresh iOS boot; report the properties only |
| wrapper `+28` differs from `0x1ff000` | the bound is written late; record it | do not infer a size from it |

**5. Health.** `sudo -n dmesg | tail -20`, then check for a new
`panic-full-*` under `/private/var/mobile/Library/Logs/CrashReporter/`. The tool
performs no write and no MMIO store, but a wrong slide is the one failure it
cannot diagnose; use `p1ctl kslide`.

**Never** run `aop-dump` with `--region-kva`/`--wrapper-kva` guessed from a
different boot; those exist to replay a *verified* capture, not to explore.

---

## 10. Verification performed in this round

Offline only; nothing was run on the iPad.

> Private graphics-dependent excerpt omitted from this source snapshot.


Results at the time of writing:

> Private graphics-dependent excerpt omitted from this source snapshot.

No device claim in §8 is a measurement; the tool exists precisely to make those
measurements. The static claims are reproducible with the commands above.

The shared Ghidra project was updated and saved (re-read-only work this round,
no deletions): `kdebug_coproc_list_add` `fffffff007578dcc`,
`RTBuddyFirmware_resolveReadAddress` `fffffff005b6fb34`,
`RTBuddyFirmware_publishVersionString` `fffffff005b6f528`,
`RTBuddy_attachFirmwareGated` `fffffff005b64720`,
`RTBuddyFirmware_allocDramTarget` `fffffff005b6737c`,
`RTBuddyFirmware_memoryMapForAddress` `fffffff005b6fd9c`,
`RTBuddyMemoryMap_initWithSegments` `fffffff005b5d968`,
`RTBuddyMemoryMap_createSegmentMapping` `fffffff005b620fc`,
`RTBuddyMemorySegment_createFromDescriptor` `fffffff005b61e78`,
`AppleA7IOPNub_withProviderPath` `fffffff005b519a0`, plus pre-comments on the
read resolver, the version publisher, the kdebug list head, the uuid candidate
table and `findUUIDHeader`.

---

## 11. Open questions after this round

| ID | Question | Decided by |
| --- | --- | --- |
| A1 | Does the firmware execute the region in place (B2) or from on-chip memory (B1)? | `aop-dump` + `compare-region.py` (§9 decision table) |
| A2 | What changed region+0 and +0x47440 after load? | A1; if B2, a diff map will show which spans were rewritten and when (`+0x47474/78` are the only *documented* writers) |
| A3 | Does `nub+0x2169` (force-DRAM) ever get set on this board? | `aop-dump` prints the segment base; a DRAM address there answers it |
| A4 | Is `+28 = 0x1ff000` the hardware value? | record it across boots with `aop-dump`; the firmware writes `+0x2c/+0x30` late, so a stable `0x1ff000` is likely its own value |
| A5 | Can a Linux-owned AOP ever be reached without an AON cycle? | unchanged: no stateless stop/ownership release exists (`ds-aop-runtime-layout.md` §5) |
| A6 | Does the AP aperture equal the AOP's view for the whole 0x9b000? | full-region dump on iOS vs the Linux R6 read of the same offsets |

**Consequence for the driver (unchanged, now better justified).** Do not
identity-check a running AOP from region offset 0. Require the properties, the
segment base/size and a full-region code-span comparison; keep every existing
refusal in place. Nothing in this round authorizes an extra AP range, a remap
write or an AOP power cycle.
