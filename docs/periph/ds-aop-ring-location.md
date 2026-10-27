# Where the AOP sensor ring lives, and where the scanner has to look

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Round 9. **Offline: no device command, no kernel edit, no commit.**
This answers the question left open by
`docs/test-logs/nextint3-2026-10-24/aop-ring-scan-early.txt` (0 hits, 14 skipped
ranges, 503.5 MiB unread, boot with no preceding iOS session).

Tags: **[V]** read from an artifact (ADT/IOService text, kernelcache
decompilation, saved capture); **[V-dev]** already verified on this iPad by a
capture on disk; **[I]** inference that fits the evidence but is not measured;
**[U]** unresolved. Same tag set as `ds-aop-running-image.md`.

Sources:

| Artifact | Path / identity |
| --- | --- |
| live IODeviceTree plane | `build/firmware/private/IODeviceTree.txt` |
| live IOService plane | `build/firmware/private/IOService.txt` |
| static ADT, decoded | `docs/adt/j71t.txt` |
| kernelcache 20H350 | `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`, SHA-256 `c6b95fcf…1e0a` |
| AppleSPU kext (local Ghidra) | `build/periph/aop-r1/kexts/AppleSPU.macho` |
| scan log | `docs/test-logs/nextint3-2026-10-24/aop-ring-scan-early.txt` |

Kernel addresses are unslid. Firmware offsets are into
`build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin`, whose linked VAs
are `0x01000000 + offset`.

---

## 0. Bottom line

> Private graphics-dependent excerpt omitted from this source snapshot.

---

## 1. Every AOP-related memory region in the ADT

### 1.1 `aop@10800000` (`IODeviceTree.txt:883–905`, static `j71t.txt:1856–1885`)

| `reg` | SoC address / size | AP PA / size (live `IODeviceMemory`, line 890) | Static ADT | Role |
| --- | --- | --- | --- | --- |
| 0 | `0x10800000 / 0x1c000` | `0x210800000 / 0x1c000` (`8866758656 / 114688`) | same | IOP wrapper: CPU control, remap, mailboxes |
| 1 | `0x10e00000 / 0x9b000` | `0x210e00000 / 0x9b000` (`8873050112 / 634880`) | `0xa0000` | firmware target / "region" |
| 2 | `0x10000600 / 4` | `0x210000600 / 4` (`8858371584 / 4`) | same | auto-boot |
| 3 | `0x10000508 / 8` | `0x210000508 / 8` (`8858371336 / 8`) | same | unresolved |

`reg` is at `IODeviceTree.txt:904`; `region-base = 0x210e00000` and
`region-size = 0x9b000` on `iop-aop-nub` are at lines 915–916 (static:
`0x210e00000 / 0xa0000`, `j71t.txt:1878/1881`). **[V]**

`aop-gpio@100F0000`: `reg = 0x100f0000 / 0x4000` → AP `0x2100f0000 / 0x4000`
(`IODeviceTree.txt:1033/1043`, `j71t.txt:1940`). 28 pins, 7 interrupt groups,
`role = "AOP"`. **[V]**

**There is no `segment-ranges`, `segment-names`, `sram-index`, `pre-loaded` or
`running` property** on either plane, and no `sram` string anywhere in the live
DT. **[V]**

### 1.2 `/chosen/carveout-memory-map` (`IODeviceTree.txt:316–356`)

This node carries the real physical addresses; `/chosen/memory-map` (line 269)
lists only names with empty values in this dump. Two entries are AOP:

| Property | Address | Size | End |
| --- | --- | --- | --- |
| `region-id-31` (line 344) | `0x210e00000` | `0x9b000` | `0x210e9b000` |
| `region-id-29` (line 326) | `0x210e9b000` | `0x5000` | `0x210ea0000` |

The two tile the static ADT's `0xa0000` aperture exactly — the `0x5000`
remainder that `ds-aop-running-image.md` §3 inferred from the iBoot data is a
real, explicit carve-out entry. Its region name is empty in this dump
(`region-name-id-29 = <00>`), so only the address identifies it. **[V]**

The same node also holds `region-id-24 = 0x800000000 / 0x7bbe4000`, i.e. a
single entry covering the whole DRAM window, and 15 further carve-outs (display,
NVMe HMB, flash regions) — **none between `0x800000000` and `0x880000000`
belongs to the AOP**. That is the negative evidence for "the ring is not a
carve-out". **[V]**

### 1.3 IOP / RTBuddy shared memory

There is **no** ADT description of a shared ring. The only AOP-visible memory
the AP describes is the firmware target (1.1). `RTBuddy`'s own mapping
(`fw+0xa8 → +0x28[0]`) maps exactly that segment. `RTBuddyVisibleMemoryImpl`
exists as a class (`initWithMemoryDescriptor(IOMemoryDescriptor*, RTBuddyCoredumpMap*, IOMapper*, IOPhysicalAddress, uint8_t)`,
string `0xfffffff005311870`) but wraps a descriptor it is given; it does not
define a fixed window. **[V]**

The per-endpoint rings are described in IOService only by their geometry, never
by address: `"slaveAlignment" = 64`, `"rx queue size" = 16384`,
`"tx queue size" = 16384` on every `AppleSPU`, including
`AppleSPU@10000007` — the `accel` endpoint 2 (`IOService.txt:1652–1656`,
`1636–1660`). **[V]**

### 1.4 One list: every AOP-related physical region on this device

| Object | AP physical address | Size | Where it comes from | Is it the ring? |
| --- | --- | --- | --- | --- |
| IOP wrapper (CPU, remap, mailbox) | `0x210800000` | `0x1c000` | `aop/reg[0]` | no |
| Firmware target / "region" | `0x210e00000` | `0x9b000` | `aop/reg[1]`, nub `region-base/size`, carveout 31 | no — staged firmware image |
| Aperture remainder | `0x210e9b000` | `0x5000` | carveout 29 | no — no consumer in any artifact |
| Auto-boot register | `0x210000600` | 4 | `aop/reg[2]` | no |
| Unidentified register pair | `0x210000508` | 8 | `aop/reg[3]` | no |
| AOP GPIO | `0x2100f0000` | `0x4000` | `aop-gpio/reg` | no |
| Mailbox FIFOs | inside the wrapper (`+0x4010`, `+0x4038`) | 4 B each | `apple-a9-aop.rst`, observer dtsi | no |
| RTBuddy firmware mapping | = firmware target | `0x9b000` | `fw+0xa8 → +0x28[0]` | no |
| **AFK buffer** (holds the rings) | **dynamic DRAM, 0x40-aligned** | ≥ 16 KiB | `0x89`/`0xa1` | contains it |
| **AFK ring** (per endpoint, per direction) | **buffer PA + 64-B multiple** | `0x4000` | `0x8a`/`0x8b`/`0x8c`, `FUN_fffffff005bab1c8` | **yes** |

Only the last two have no ADT address, and they are the ones the scanner wants.
**[V]**

---

## 2. How iOS allocates the ring

**Verdict:** a **kernel heap allocation** handed to the AOP through the
RTBuddy/AFK `0x89` → `0xa1` message. Not a fixed carve-out from the ADT, not
firmware-internal SRAM, not the `region-base` window. Evidence below. **[V]**

### 2.1 The message exchange

The AOP asks the AP for a buffer; the AP allocates it and returns the physical
address. From the AFK endpoint switch `FUN_fffffff005ba88e0` (extracted-kext
decompilation, case at `0xfffffff005ba8bb4`): **[V]**

```text
in  0x89 word = (0x89 << 48) | (blocks << 16) | tag      size = blocks * 64
out 0xa1 word = (0xa1 << 48) | (pa & 0x0000ffffffffffff)
```

The handler rounds the requested size up to the cache line (64 B), calls
`provider->vtable[0x558](../release/OMISSIONS.md)`, then reads the new descriptor's
`+0x78` (kernel VA) and `+0x80` (physical address), and stores
`{tag, descriptor, va, pa, size}` in a five-word table.

### 2.2 The allocation path (all hops verified)

| Hop | Address | What it proves |
| --- | --- | --- |
| provider slot | `0xfffffff006d4c558` → `0xfffffff005b671b4` | endpoint-provider (RTBuddy) method |
| action | `FUN_fffffff005b67264` | calls the allocator with `... , 0x40)` |
| allocator | `FUN_fffffff005b6d000` | OSMetaClass alloc + vtable `+0xa0` = `init` |
| init | `IOBufferMemoryDescriptor::initWithPhysicalMask` `0xfffffff0077757d4` | `_kernel_map`, `param_3` capacity, **one contiguous range** |
| backing | `FUN_fffffff00772a3d0` → `FUN_fffffff00725c844` | `kmem_alloc_contig`-class allocation in `kernel_map` |

Consequences, all **[V]**:

* the buffer is **kernel DRAM**, wired and **physically contiguous** (one
  `IOGeneralMemoryDescriptor` range with a single physical segment);
* its alignment is **0x40** (the `0x40` argument is threaded from
  `FUN_fffffff005b67264`);
* its address is chosen by the iOS page allocator at message time — **it is not
  a carve-out, not `region-base`, not AOP SRAM**;
* the value sent back to the firmware is the **raw 48-bit AP physical
  address**, not an IOVA (there is no IOMMU in this path; the AOP's own
  translation `0x0103028c` only adds `delta` for addresses inside the firmware
  window, so `0x8…` passes through unchanged).

### 2.3 The AP-side table that names the address

`AppleSPU` keeps the `0x89` results in a fixed table (from the same
decompilation): **[V]**

```text
count        AppleSPU + 0x508          (u32, max 16)
entry[i]     AppleSPU + 0x288 + 0x28*i
  +0x00  u16  tag
  +0x08  ptr  IOBufferMemoryDescriptor
  +0x10  u64  kernel VA
  +0x18  u64  physical address   <-- what 0xa1 sent
  +0x20  u64  size
```

The ring itself is a sub-range of that buffer. The firmware follows with
`0x8a`/`0x8b`/`0x8c` = `(type << 48) | (offset_blocks << 32) | (size_blocks << 16) | tag`,
and the AP builds a ring object at `buffer_va + offset`
(`FUN_fffffff005baaf2c` → `FUN_fffffff005b8465c` → `FUN_fffffff005bab1c8`).
Because the buffer is contiguous, **ring PA = buffer PA + offset**. **[V]**

`0x8b` is the IOP→AP ring (the one that carries reports, created at
`AppleSPU+0xd0`); `0x8a` is AP→IOP and `0x8c` historical
(`ds-aop-accel.md` §4.2). **[V]**

### 2.4 Size

For the accel endpoint, `rx queue size = 16384`, `slaveAlignment = 64`
(`IOService.txt:1652–1656`). A 16 KiB ring therefore has `bufsize = 0x3F40`,
header `0xC0`, entries 64-byte aligned. **[V]**

### 2.5 The header is the scanner's signature

`FUN_fffffff005bab1c8` (called by `FUN_fffffff005b8465c`, itself called by the
ring constructor `FUN_fffffff005baaf2c`) with `block = slaveAlignment = 0x40`
writes: **[V]**

| Offset | Value written | Code |
| --- | --- | --- |
| `+0x00` | `size − 3*0x40` = `size − 0xC0` | `piVar9[0] = param_4 - 0xC0` |
| `+0x04` | `0x00070006` | `piVar9[1] = param_6 \| param_8<<16` = `6 \| 7<<16` |
| `+0x40` | `0` (rptr) | `*puVar1 = 0`, `puVar1 = base + 0x40` |
| `+0x80` | `0` (wptr) | `*(puVar1 + 0x40) = 0` |
| `+0xC0` | entries | payload area |

This is byte-for-byte the `a9-aop-ring.h` signature
(`A9_RR_BUFSIZE 0x3f40`, `A9_RR_MARKER 0x00070006`, rptr/wptr). The scanner is
looking for the right object. **[V]**

---

## 3. Mapping against the scanner's skip list

### 3.1 Address spaces

| Space | Range | In the scanner? |
| --- | --- | --- |
| Linux `System RAM` | `0x800000000` … top (`ram_bytes = 2,076,049,408`) | **yes** (74.6 % read) |
| AOP firmware window | `0x210e00000 / 0x9b000` + `0x210e9b000 / 0x5000` | **no** — not `System RAM`, not in the linear map |
| AOP wrapper / GPIO / auto-boot | `0x210800000`, `0x2100f0000`, `0x210000600`, `0x210000508` | **no** — MMIO |

So the ring, being a DRAM allocation, is **inside the address space the scanner
already walks**. Adding the AOP window would not find it; it is the firmware
staging area, and the scanner would have to `ioremap` it because it is not in
the linear map. **[V]**

### 3.2 The 14 skipped ranges (`aop-ring-scan-early.txt:13–26`)

| Range | Size | What it is | Could hold an intact ring? |
| --- | --- | --- | --- |
| `0x800000000–0x80200000` | 32 MiB | CMA (16 MiB at the RAM base, `CONFIG_CMA_SIZE_MBYTES=16`) + adjacent early reservations | **yes**, until a CMA user allocates — Linux does not write it at boot |
| `0x8040c8000–0x8040ec000` | 144 KiB | `flash@…` reserved-memory node (same shape as `flash@80561c000` in the older dmesg) | unlikely, 144 KiB |
| `0x804474000–0x8045b0000` | 1.23 MiB | platform reservation | unlikely |
| `0x806000000–0x8078d0000` | 24.8 MiB | platform reservation | unlikely |
| `0x807a00000–0x807e34000` | 4.20 MiB | platform reservation | unlikely |
| `0x807e38000–0x807e48000` | 64 KiB | platform reservation | no |
| `0x807e4c000–0x807e74000` | 160 KiB | platform reservation | no |
| `0x860314000–0x87b800000` | **436.9 MiB** | m1n1 payload (kernel/DTB/initramfs, ≈180.9 MiB, **already overwritten**) + crash-kernel reservation (256 MiB, **never written**) | **yes, in the crash-kernel part** |
| `0x87ba00000–0x87bbdc000` | 1.86 MiB | platform reservation | unlikely |
| `0x87bbe0000–0x87bbe4000` | 16 KiB | `flash@87bbe0000` reserved-memory node | no |
| `0x87b800000–0x87ba00000` (no-map) | 2 MiB | `ramoops` (pstore) | no — no-map, actively written |
| 3 × reader/stack | 96 KiB | the scanner itself | excluded by design |

Totals: walked 1,548,091,392 B (74.6 %), skipped 527,958,016 B (503.5 MiB), of
which 501.4 MiB is `reserved`, 2 MiB `no-map`, 96 KiB reader. **[V]**

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

### 3.3 The Linux `/reserved-memory` nodes that matter

m1n1/our DT create five static nodes on this port; three are inside
`System RAM` and therefore in the scanner's address space at all: **[V]**

| Node | PA / size | Flags | In the scan's range? |
| --- | --- | --- | --- |
| `ramoops@87b800000` | `0x87b800000 / 2 MiB` | `no-map` | yes — skipped as `no-map` |
| `flash@87bbe0000` | `0x87bbe0000 / 16 KiB` | reserved | yes — skipped as `reserved` |
| `flash@80561c000` (this boot: `0x8040c8000`) | 144 KiB | reserved | yes — skipped as `reserved` |
| `framebuffer@87c95c000` | `0x87c95c000 / 36 MiB` | `no-map` | **no** — above the top of RAM |
| `nvme-hmb@0` | `0x87ee00000 / 18 MiB` | `no-map` | **no** — above the top of RAM |

None of them is the ring; the framebuffer and HMB are not even in DRAM that
Linux describes as memory. The ring is not covered by any `/reserved-memory`
node, which is exactly why it is unprotected. **[V]**

### 3.4 Why the 2026-10-24 run was always going to be 0

The ring is allocated **per AFK session**, at `0x89` time, inside whatever DRAM
the iOS allocator hands out. A boot that starts with DFU from a running Linux
kernel has:

* no iOS session to have allocated a buffer,
* no `0x89`/`0xa1` handshake,
* and therefore no header anywhere in DRAM, whatever the scan coverage.

Additionally, the previous Linux boots ran only the observer, which never sends
`0x89`. **[V]** (from the driver model) + **[I]** (session history).

The same log's `hits=0` is therefore a statement about the boot, not about the
hypothesis. The `.rst` already requires "the FIRST Linux boot after iOS → DFU →
Linux without an AON power cycle" (`apple-a9-aop.rst`, E1 section); this run
did not satisfy it. **[V]**

### 3.5 What would the scanner have to read?

Only DRAM — the same `0x800000000…` span it already walks. Concretely:

1. the already-walked free `System RAM` (correct, keep),
2. **plus** the reserved-but-linear-mapped DRAM Linux never writes: the
   crash-kernel reservation and the CMA area,
3. **never** the AOP window, the wrapper, the GPIO block, or any `no-map` range.

All of these are ordinary DRAM (page-cacheable, no read side effects), so
`copy_from_kernel_nofault` through the linear map is safe; none of them is MMIO
or SRAM. Nothing in the ring's construction puts it in MMIO or SRAM, so no
`ioremap`-based range is needed. **[V]** for the memory types, **[I]** for the
"never written" part.

---

## 4. The SRAM question

It is not SRAM, so the "exact physical window" does not exist for the ring.
Two adjacent facts are still worth pinning down:

* **The AOP does have an SRAM block**, and PMGR exposes it as `PMS_SRAM`
  (`docs/adt/j71t.txt:798`, id1 = 55, next to `AOP_GPIO`/`AOP_CPU`/
  `AOP_FILTER`/`AOP_BUSIF` at lines 666–702). **[V]**
* **No AP aperture for it is described anywhere** — not in `aop/reg`, not in
  `carveout-memory-map`, not as a `segment-ranges` entry. If the firmware keeps
  a sample copy there, the AP cannot read it. **[V]** + **[I]**

The only firmware-private window the AP *can* map is `0x210e00000 / 0x9b000`
(+ `0x210e9b000 / 0x5000`). Linux maps it read-only today through the active
driver's `region` resource and Phase A reads of head/UUID/build/tags
(`apple-a9-aop.rst`, Phase A). It is mappable read-only, but it is **not** the
ring: it is the staging target for the firmware image (`ds-aop-running-image.md`
§6.1), and no code path in AppleSPU's AFK layer references it. **[V]**

---

## 5. Recommendation for the scanner

### R1 — Only scan in a boot that can contain a ring (necessary condition)

**Do:** keep `ring_scan_early=1`, but require the documented session: iOS boot
with accel enabled → DFU → palera1n → pongoOS → m1n1 → Linux, no AON power
cycle. **Evidence:** the ring is created by the `0x89`/`0xa1` exchange (§2.1),
so it exists only if an AP session allocated it; the AOP world survives AP
resets but the ring does not get re-created. **[V]/[I]**

**Do not** expect any result from a Linux→DFU boot (§3.4).

### R2 — Close the coverage hole: read linear-mapped reserved DRAM

**Do:** admit memblock-reserved, linear-mapped `System RAM` pages as
**read-only** scan sources. Minimum addition on this boot:

| Range | Size | Why |
| --- | --- | --- |
| `0x86b800000–0x87b800000` | 256 MiB | crash-kernel reservation (`crashkernel=256M`); reserved before `postcore_initcall` and never written by Linux until a panic |
| `0x800000000–0x801000000` | 16 MiB | CMA at the RAM base; reserved, untouched at scan time |

**Evidence / safety:** these are normal DRAM pages (`pfn_valid`,
`virt_addr_valid`, linear map), not MMIO, so `copy_from_kernel_nofault` is as
safe there as in free RAM. Keep the existing exclusions for `no-map`, invalid
linear-map pages, and the reader's own pages/stack. **[V]** for the memory
types, **[I]** for "untouched at scan time" (true until a CMA consumer or a
panic-kexec exists).

**Do not** add `0x210e00000`/`0x210e9b000` (§4): wrong object, not linear-mapped,
and it would need a new `ioremap` path for zero expected yield.

**Expected gain:** admitting only the crash-kernel reservation and CMA lifts
coverage from 74.6 % to ~88 % of RAM; admitting every reserved non-`no-map`
page (including the already-overwritten payload) reaches ~99.9 %, with only the
2 MiB `ramoops` left out. The part that can actually hold an *intact* ring is
the 272 MiB crash-kernel + CMA slice; the payload slice is readable but its
content was overwritten by Linux itself. **[I]**

The driver change is small: the page classifier currently records
`A9_RR_SKIP_RESERVED` for every `memblock` reservation; it would instead admit
the resource named `Crash kernel` (and the CMA area) and keep the skip for the
`no-map` flag. It does not need a new mapping.

### R3 — Prefer the deterministic PA, but it is blocked today

**Do:** instead of scanning, read the address from iOS: `AppleSPU+0x508`
(count) and `+0x288 + 0x28*i` (tag/va/**pa**/size), which is the value that was
sent in `0xa1`. With that PA and the `0x8b` offset, the ring PA is exact, and it
can be placed in the m1n1 payload DT as a reserved region before Linux boots.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

### R4 — Keep the positive identification strict

A 16 KiB block with `0x3F40/0x00070006` is necessary but not sufficient: every
endpoint that published an `AppleSPU` (1–6, 9, 13, 14, 17, 19 in the live tree)
has its own rx/tx rings, so a scan in a live session may hit half a dozen.
Require (a) a `wptr` advance inside the 2 s watch and (b) a `0xc2` subtype in a
parsed entry before calling it the accel ring — exactly what E1/E2 already do.
**[V]** for the multi-ring topology, **[I]** for the hit rate.

### R5 — Two caveats for E2 if a ring is found

* If the hit is inside the crash-kernel or CMA reservation, the current E2
  contract (a plain, available, `no-map` `/reserved-memory` child) cannot
  express it; that reservation is neither `no-map` nor free. E2 would need a
  new "reserved but inherited" case.
* Reserving one accel ring does not contain the other endpoints' rings: the AOP
  may still be DMA-writing into pages Linux owns. The `.rst` already says this;
  it is worth repeating in any report that claims a live ring. **[V]**

---

## 6. Verification performed (offline)

```sh
# ADT planes
python3 - <<'PY'   # decoded every carveout-memory-map region-id
...
PY
grep -n "aop\|region-base\|region-size" build/firmware/private/IODeviceTree.txt
sed -n 1856,1945p docs/adt/j71t.txt

# kernelcache (shared Ghidra project, unslid)
python3 scripts/ghidra.py decompile_function_by_address address=0xfffffff005b67264  # 0x89 provider action
python3 scripts/ghidra.py decompile_function_by_address address=0xfffffff005b6d000  # alloc + init
python3 scripts/ghidra.py decompile_function_by_address address=0xfffffff0077757d4  # IOBufferMemoryDescriptor::initWithPhysicalMask
python3 scripts/ghidra.py decompile_function_by_address address=0xfffffff00772a3d0  # contiguous backing
python3 scripts/ghidra.py get_xrefs_to address=0xfffffff005b67264                  # vtable slot 0xfffffff006d4c558

# AFK ring code (local extractor; the shared project lacks 0x5ba88e0/0x5baaf2c)
~/.local/share/uv/tools/ghidra-mcp/bin/python scripts/periph/ghidra-local.py \
  --binary build/periph/aop-r1/kexts/AppleSPU.macho \
  --output build/periph/aop-ring-loc/ringinit --function 0xfffffff005baaf2c
~/.local/share/uv/tools/ghidra-mcp/bin/python scripts/periph/ghidra-local.py \
  --binary build/periph/aop-r1/kexts/AppleSPU.macho \
  --output build/periph/aop-ring-loc/ringhdr --function 0xfffffff005bab1c8
```

Results: the 0x89 handler allocates through `+0x558` and replies `0xa1 | pa`
(§2.1); `initWithPhysicalMask` allocates one contiguous `kernel_map` range
(§2.2); `FUN_fffffff005bab1c8` writes `size-0xC0`, `0x00070006`, and zeroes
`+0x40`/`+0x80` (§2.5). No device claim in this document is a measurement; the
iOS-side PA in §5 R3 was **not** read.

The shared Ghidra project was only read. Local extractor output went to the
gitignored `build/periph/aop-ring-loc/`.

---

## 7. Open questions

| ID | Question | Decides it |
| --- | --- | --- |
| B1 | Does DRAM content survive DFU → checkm8 → pongoOS → m1n1? | a first-boot scan hit, or an iOS-side PA plus the same bytes found on the Linux side |
| B2 | Does the AOP keep writing after the AP disappears? | E1 `wptr` advance (unchanged) |
| B3 | Where does the iOS allocator put the 0x89 buffer on this device? | iOS buffer table read (§5 R3) or a lucky scan in the right boot |
| B4 | Are the crash-kernel pages linear-mapped on this port? | `/proc/iomem` + `virt_addr_valid` at the scan phase |
| B5 | Is there a class-checked anchor for the iOS kernel walk? | **answered for the AppleSPU path**: `gRegistryRoot` `DAT_fffffff0078e6cc8` + per-hop vtable/ID checks, implemented as `aop-ring` (§8); the kdebug walk stays disabled |

**Consequence for the plan.** The ring is a DRAM object with a dynamic address,
so no amount of ADT work will name it. Priority order: (1) run E1 in a boot
that follows an iOS session, (2) close the reserved-DRAM coverage hole so the
scan is actually complete, (3) only then invest in the deterministic iOS-side
PA read, which is the only way to make E2 repeatable across boots.

---

## 8. The deterministic PA read, implemented (`aop-ring`)

2026-10-24. **Offline: kernelcache only, no device run, no commit.** This
closes R3 for the PA read and answers B5 for this path. The old kdebug walk
stays disabled; nothing below touches it.

> Private graphics-dependent excerpt omitted from this source snapshot.

### 8.1 The anchor is the IORegistry root, not the kdebug list

| hop | address / field | evidence |
| --- | --- | --- |
| anchor | `gRegistryRoot` at `0xfffffff0078e6cc8 + slide` | `IORegistryEntry::initialize` `0xfffffff007736ccc` allocates the 0x28-byte root and stores vtable `0xfffffff0071279c8`; `getRegistryRoot` `0xfffffff007736f50` returns that variable |
| links | `entry+0x18` → `OSDictionary` | `makeLink` `0xfffffff007734a14` reads `*(entry+0x18)`; `attachToParent` `0xfffffff007732c9c` does the same at `+0x20` |
| value | `OSArray` (`+0x14` count, `+0x20` backing) | same call sites (`OSDictionary::getObject` → `OSArray::getCount`) |
| identity | `*(*(entry+0x10)+8)` = registry entry ID | `IORegistryEntry::getRegistryEntryID` `0xfffffff007737ac0` |

> Private graphics-dependent excerpt omitted from this source snapshot.

The live chain, from `IOService.txt` and the same references in the kernel:

```text
Root -> J71tAP [IOPlatformExpertDevice] -> AppleARMPE -> arm-io@2240000
     [IOPlatformDevice] -> AppleS8000IO -> aop@10800000 [AppleARMIODevice]
     -> AppleA7IOPV2 -> iop-aop-nub [AppleA7IOPNub] -> RTBuddy
     -> AOPEndpoint2 [RTBuddyEndpointService] -> AppleSPU@10000007
```

That is 11 hops (`Root` is the first). All 11 instance vtables were resolved
from the kexts' `OSMetaClass` registrations and their `MetaClass::alloc`
(which calls the constructor that stores the vtable):
`IORegistryEntry` `0xfffffff0071279c8`, `IOPlatformExpertDevice`
`0xfffffff0071283c0`, `AppleARMPE` `0xfffffff006cebe38`, `IOPlatformDevice`
`0xfffffff0071289b0`, `AppleS8000IO` `0xfffffff006ee5d68`,
`AppleARMIODevice` `0xfffffff006ce9f38`, `AppleA7IOPV2` `0xfffffff006d3ff98`,
`AppleA7IOPNub` `0xfffffff006d442d8`, `RTBuddy` `0xfffffff006d4c000`,
`RTBuddyEndpointService` `0xfffffff006d4c900`, `AppleSPU`
`0xfffffff006d62fe0`. **[V]**

### 8.2 The table, corrected

§2.3 labelled `+0x10` "kernel VA". The handler's own stores say otherwise.
From `FUN_fffffff005ba88e0` (0x89 case at `0xfffffff005ba8bb4`):

```text
plVar16 = provider->vt[0x558](../release/OMISSIONS.md);
param_1[i*5 + 0x52] = plVar16;                 // +0x08  the 0x89 allocation
param_1[i*5 + 0x53] = plVar16->vt[0x78]();     // +0x10  descriptor
param_1[i*5 + 0x54] = plVar16->vt[0x80]();     // +0x18  physical address
param_1[i*5 + 0x55] = size;                    // +0x20  requested size
count at +0xa1 (long units = +0x508), bound `count < 0x10`
```

The allocation is `RTBuddySlaveMemoryBuffer` (metaclass
`0xfffffff007a10650`, size 0x38, vtable `0xfffffff006d4dad0`); its accessors
are `0xfffffff005b1fd58` (`return *(u64*)(this+0x10)`) and
`0xfffffff005b1fd60` (`return *(u64*)(this+0x28)`), and the creation path
`0xfffffff005b1fb5c` (slot `0xfffffff006d336d0+0xa8`) calls
`IOBufferMemoryDescriptor::withOptions` and stores the descriptor at `+0x10`
and the PA at `+0x28`. So the table is:

| offset | field |
| --- | --- |
| `+0x00` | u16 tag |
| `+0x08` | `RTBuddySlaveMemoryBuffer*` |
| `+0x10` | `IOBufferMemoryDescriptor*` (buffer+0x10) |
| `+0x18` | **u64 physical address** (buffer+0x28; the `0xa1` value) |
| `+0x20` | u64 requested size |

**[V]** The PA is unchanged from §2.3; only the two object/VA labels move.

The VA is recovered by replaying
`IOBufferMemoryDescriptor::getBytesNoCopy()` `0xfffffff007775e04`:
`(flags at +0x20 & 0xf0) == 0x20 ? *(u64*)(desc+0xb8) : *(u64*)(*(u64*)(desc+0x60))`,
length at `+0x50`. It is only used when `buffer+0x10 == desc`,
`buffer+0x28 == pa`, `desc` is exactly an `IOBufferMemoryDescriptor`
(vtable `0xfffffff00715dcb0`, size 0xe8), the VA is non-zero, page-aligned,
`size` fits the length, and `kva(VA, 0x100)` holds. Otherwise the tool prints
PA/size and says why. **No VA is ever guessed from the PA.** **[V]** for the
accessor, **[I]** for "the 0x89 buffer's first 0x100 bytes contain the ring
header" (see 8.4).

### 8.3 Run order

> Private graphics-dependent excerpt omitted from this source snapshot.


The report ends with one `PA buf[i] pa=0x... size=0x...` line per entry, and
the second read reports `rptr`/`wptr` before/after plus the byte diff. A moved
`wptr` is the direct answer to B2 for this session.

### 8.4 How the orchestrator uses the PA

1. On iOS, with the accel interface open (the default pick), run the two
   commands above. Record `pa` and `size`; `size` should be `0x4000`.
2. Go **iOS → DFU → palera1n → pongoOS → m1n1 → Linux** without an AON power
   cycle. This is the §3.4 session condition; a Linux→DFU boot has no ring.
3. On the first Linux boot, before anything else allocates, read exactly
   `pa .. pa+0x100` **read-only** and compare:
   `+0x00 == size-0xC0` (usually `0x3f40`), `+0x04 == 0x00070006`,
   `+0x40` = rptr, `+0x80` = wptr, both `0x3f40`-bounded and 64-aligned.
   Do not scan and do not `ioremap` blindly: the buffer was a normal
   `kernel_map` allocation, so `pfn_valid`/`virt_addr_valid` on the page and
   the linear map are the right access. If the page is not `System RAM` on
   that boot, stop and report; that is data for B1, not a reason to probe.
4. Then — and only then — place `pa,size` in the m1n1 payload DT as a reserved
   region (R5's "reserved but inherited" case) so the AOP cannot be DMA'd
   over while Linux owns the page.

### 8.5 What §8 does **not** establish

* The ring may sit at a non-zero offset inside the 0x89 buffer: the 0x8b
  message carries `offset_blocks`, and the constructor adds it to
  `getBytesNoCopy()`. If the header is not at buffer `+0x00`, the tool prints
  `signature NOT matched` while still printing the correct PA. The PA of the
  **ring** is then `PA buffer + offset`, and the offset is not in this table.
  **[U]** until a device run says whether the accel ring starts at 0.
* `count == 0` is a valid, useful result: the accel endpoint never sent
  `0x89` in that session (no HID client opened it). No PA exists then.
* B1 (does DRAM survive DFU) and B2 (does the AOP keep writing) remain open;
  §8 only makes the address exact.
* Not run on a device. The host test pins the resolver and the refusal rules,
  not the live layout.

