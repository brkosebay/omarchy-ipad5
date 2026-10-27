# The warm-boot DART capture: what `err 0x500 addr 0x3bfff0d0` actually is

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Analysis-only sub-agent report. No device access, no kernel edits, no commits.
Sources: the shared Ghidra iOS 16.7.10 j71t kernelcache (`AppleS5L8960XDART`,
`IONVMeFamily`), the `a9/boot` tree in the Docker volume, `patches/boot/`,
`patches/pcie/`, and the logs on this Mac. Every claim is marked **verified**
(address, file:line, or log line) or **inferred**. Where a number could only be
read from the photo I say so and give the crop I used.

Companion documents: `2026-10-23-round7-warm3-rescue.md` (the capture),
`ds-nvme-c-forensics.md` (boot-to-boot drift), `ds-nvme-b-ios-ans-restart.md`
(iOS ANS restart), `docs/pcie/enable-freeze.md` (round-3 DART RE).

---

## 0. TL;DR

1. **The `err` register is not a fault latch in the sense the round-7 write-up
   assumed.** `0x500` decodes as *"last recorded access was a write"* plus a
   constant attribute bit; the fault-valid bit (31) and the whole error-code
   field (bits 6:0) are **clear**. Apple's own handler treats exactly that
   combination as *not a fault*, and would log "spurious DART/SMMU interrupt".
   **verified**, §2.2/§2.5.
2. **No capture of any warm boot — healthy or failing — ever shows bit 31
   set.** The only bit-31 samples in this repo are `r3-hold`/`r4-integ6`
   (`0x80000102`), i.e. the era with the known-broken STE encoding. **verified**,
   §4.
3. So the write at `0x3bfff0d0` was **not** a DART translation or protection
   fault: the DART translated it and performed it. The address is still the most
   valuable datum in the capture — it is admin CQ entry 13, and the ANS snapshot
   taken at the same moment shows that entry **empty** (`next status 0x0 cid
   0x0`). A completion write that the DART performed but the driver never saw.
   **verified** for the arithmetic and the queue state, **inferred** for the
   sequence.
4. **Most likely mechanism: a stale translation for the CQ page on a warm boot**
   (§6.1). The DART keeps a TLB entry from the previous kernel for DVA
   `0x3bfff000`; Linux's only flush is the single, unchecked one at attach
   (`§5.3`), and its reset path *cannot* flush at all (`§5.2`). The completion
   is written into the previous kernel's page, which this kernel has reused —
   a successful translation, hence no fault, hence no bit 31. It needs the
   backing page to move between boots, which is why it is roughly a 10 %-per-jump
   hazard and not a threshold.
5. Two code defects make that possible and both are cheap to fix: the flush
   result is discarded, and the S5L8960X reset path returns before flushing.
   **verified**, §5.2/§5.3.
6. A second, unrelated defect is why the capture is so thin: **Linux acks the
   DART error register without ever printing it** (`attach_config()` and
   `hw_reset()` W1C `+0x10` silently). That is the one change that makes the
   next failure conclusive. **verified**, §5.1.
7. iOS has neither window: it reprograms the whole DART (reflector → stream
   setup → TCR → fetch → diag) on **every** activation, i.e. every port
   power-up, and it **panics** on any fault. **verified**, §2.7.

The proposed change (§7) is therefore: make the DART state explicit and observed
at every controller start — quiesce, flush (with the enable→invalidate ordering
this DART requires), clear **after logging**, re-apply in Apple's order, program
TTBRs, enable, verify, and fail closed on any step that does not confirm.

---

## 1. The capture, re-read from the photo

The transcription in `2026-10-23-round7-warm3-rescue.md` is accurate for the
rescue block; two digits in the TTBR field are not readable, and one early TTBR
in the transcription is wrong. I re-cropped and enlarged the JPEG
(`2026-10-23-round7-warm3-rescue.jpg`, 1500×2000) rather than trusting the
transcription.

**Rescue line (verified by eye, `host rescue: dart …`):**

```
dart tcr 0x80 err 0x500 addr 0x3bfff0d0 diag 0x100 reflector 0x20fffc
     fetch 0xe0303 ttbr 0x8000cb50 0x8000cbcc 0x000… 0x8000c…
```

- `err 0x500` and `addr 0x3bfff0d0` are unambiguous (crops at 14–16× show
  `err 0x500 addr 0x3bfff0d0` cleanly). There is **no** leading `0x8…`, i.e.
  bit 31 is really clear.
- `tcr 0x80`, `diag 0x100`, `reflector 0x20fffc`, `fetch 0xe0303` match every
  other boot exactly.
- The four TTBRs read `0x8000cb50 0x8000cbcc 0x000… 0x8000c…` with at least
  one nibble missing from each value at this scale. What *is* readable is that
  they differ from the same boot's early dump, which is the usable fact
  (see §4.3).

**Early line of the same boot (verified):**

```
host after watch: dart tcr 0x80 err 0x100 addr 0x1e0f000 diag 0x100
     reflector 0x20fffc fetch 0xe0303 ttbr 0x8080cebc 0x8080c8bc 0x80807cb4 0x8080c8ac
```

The transcription's `0x8000cebc …` is a lost `8`: at 16× the first token reads
`0x8080cebc`, identical to the healthy N=2 boot. This matters because
`ds-nvme-c-forensics.md` §3.4 builds an argument on the "one digit shorter"
value; with the digit present, the early state of the failing boot is
**bit-identical to the healthy N=2 boot**, which is what its own table already
says for every other field.

---

## 2. The iOS DART (`AppleS5L8960XDART`) register model

All addresses below are in the shared Ghidra project
(`kernelcache.release.iPad6,11_12`, image base `fffffff005298000`). This is the
kext that owns the `apple,s5l8960x-dart` nodes, including the two behind the
PCIe ports (`601008000.iommu` = port 0/NVMe, `602008000.iommu` = port 1/Wi-Fi).

### 2.1 Register map (verified)

| Offset | Name | Notes |
|---|---|---|
| `0x000` | stream command | `sidmap << 8 \| cmd`; `BIT(1)` = invalidate, `BIT(3)` = busy |
| `0x00c` | TCR | one **byte per stream**; `0x80` = translate enable |
| `0x010` | error/status | see §2.2 |
| `0x01c` | error address | DART-side address (window-relative) |
| `0x020` | diag-config | ADT `diag-config`, j71t `0x100` |
| `0x024` | error reflector | ADT `error-reflector >> 12`, j71t `0x20fffc` |
| `0x030` | fetch-config | ADT `fetch-config`, j71t `0xe0303` |
| `0x040 + 16·sid + 4·i` | TTBR[i] | `BIT(31) \| (PA >> 12)` |
| `0x1000…0x1018` | second page | perf counters / interface-test status |

Linux carries the identical map: `drivers/iommu/apple-dart.c:54-82`
(`DART_S5L8960X_*`), and the h9p diagnostics copy it at
`drivers/pci/controller/pcie-apple-h9p.c:290-297`.

### 2.2 `+0x10` — error/status, decoded from Apple's handler (verified)

The decoder is `FUN_fffffff005fb47bc`; the IRQ action that feeds it is
`FUN_fffffff005fb2e04`; the strings are at `fffffff0054bee1a…fffffff0054bee8c`.

```
FUN_fffffff005fb2e04:                      ; IRQ action
  ldr  w9, [dart, #0x10]
  tbnz w9, #31, fault_path                 ; bit31 == "exception valid"
  … "Spurious DART/SMMU interrupt" …       ; clears only the +0x1000 page
```

```
FUN_fffffff005fb47bc:                      ; decode + panic
  status = rec->status                    ; = readl(dart+0x10)
  sid    = (status >> 24) & 3
  is_dart? bit0 -> "TTBR invalid"
           bit1 -> "STE invalid"          ; fffffff0054bee27
           bit2 -> "PTE invalid"          ; fffffff0054bee33
           bit3 -> "write protect"        ; fffffff0054bee3f
           bit4 -> "read protect"         ; fffffff0054bee4d
           bit7 -> "region protect"       ; fffffff0054bee5a
           bits6:5 == 3 -> "AXI decode"   ; fffffff0054bee73
           bits6:5 == 2 -> "AXI error"    ; fffffff0054bee69
           else -> "unknown"              ; fffffff0054bee7e
  dir = (status & 0x400) ? "write" : "read"   ; fffffff0054bee86 / …8c
  …
  writel(status & 0x7f, dart + 0x10)       ; ack: W1C of bits 6:0 only
```

So, for the S5L8960X PCIe DART:

| Bits | Meaning | Evidence |
|---|---|---|
| `6:0` | error code (TTBR/STE/PTE/protection/AXI) | decode above |
| `7` | region protect | `tbnz w23,#7` at `0x5fb4974` |
| `8` | **not decoded by Apple** — set in every record observed | §4 |
| `10` (`0x400`) | direction: 1 = write, 0 = read | `tst w23,#0x400` at `0x5fb49c4` |
| `25:24` | stream/SID | `(status >> 0x18) & 3` |
| `31` | exception valid (the IRQ gate) | `tbnz w9,#0x1f` in the action |

**The ack mask is the key detail:** Apple clears `status & 0x7f`, i.e. bits 6:0
only. Bits 8 and 10 are *not* W1C — they are attributes of whatever the
register last recorded, and they persist until the hardware records something
new. That is exactly what the logs show (bit 8 set in every boot, bit 10
toggling between samples).

### 2.3 `+0x1c` — the address is DART-side, not an IOVA (verified)

`FUN_fffffff005fb2e04` loads `+0x1c` into the record's address field and the
handler prints it as the "DVA". Linux does the same and adds the window base
back: `apple_dart_s5l8960x_irq()` prints both (`apple-dart.c:1333-1337`), and
the round-3 fault proved the hardware indexes the tables with
`IOVA - 0x80000000` (`enable-freeze.md`, r3-hold: `at 0x3bffe000` for IOVA
`0xbbffe000`).

For this capture: `0x3bfff0d0 | 0x80000000 = 0xbbfff0d0`, which is the admin CQ
(`0xbbfff000`) + `0xd0` = **entry 13** of 16-byte completion entries. The ANS
snapshot in the same photo prints `CQ 0x00000000bbfff000`. **verified** (both
sides of the sum are in the photo).

### 2.4 The second page, `+0x1000` (verified, not relevant here but easy to confuse)

The IRQ action's spurious path reads `+0x1000` and W1Cs bits 20/22/23/25 through
`FUN_fffffff005fb5374`; `FUN_fffffff005fb5448` does the same for the secondary
MMU at `+0xb28`. `FUN_fffffff005fb4b00` is the "interface test" self-test that
writes synthetic entries to `+0x000` when `enable-perf-counters` is set
(strings `InterfaceTest`, `AXIDecode`, `AXIError` at
`fffffff0054be8e6…fffffff0054be8f9` are published as IORegistry names, not used
for decode). j71t's ADT lists `diag-config`, `fetch-config`,
`pte-protection-override` and `error-reflector` but no `enable-perf-counters`
(inferred from the property table in `enable-freeze.md`), so none of this runs.
Linux never touches `+0x1000` at all.

### 2.5 What iOS considers a fault, and what it does (verified)

- `bit31` clear ⇒ the action logs `"Spurious DART/SMMU interrupt"`, acks only the
  `+0x1000` page, and returns. It does **not** ack `+0x10`. So a bit31-clear
  status is sticky, by design.
- `bit31` set ⇒ decode, then `_panic()` with
  `"DART error: SID %u %s exception on %s with DVA %#x (TTBR %u SEG %#x PTE %#x) ERROR_STATUS 0x%08x TIME %#llx AXI_ID %#x"`
  (`fffffff0054bee91`). There is **no recovery path** for a DART fault in
  iOS: one fault ends the boot.
- There is one gate before the panic: `FUN_fffffff005fb47bc` first calls
  vtable `+0x678` and checks `diag_config & BIT(9)` (read from the DART base
  at `+0xaf0`). With j71t's
  `diag-config = 0x100` that bit is 0, so the panic path is taken. The
  alternative path acks `status & 0x7f` and continues — that is Apple's
  factory "soft fault" mode, not something a normal boot uses.

### 2.6 TLB invalidation on this DART (verified)

The S5L8960X has **no** standalone flush: invalidation is a stream command, and
Apple's `AppleS5L8960XDART` only issues it with translation enabled (this is the
source of the comment in Linux at `apple-dart.c:632-639`). The command is
`writel(sidmap << 8 | BIT(1), dart+0)` then poll `BIT(3)` clear. Linux does this
at `apple-dart.c:468-496` with a 100 µs budget, and the caller
(`apple_dart_setup_translation`, `apple-dart.c:764`) **discards the return
value**: a stuck busy bit prints one `dev_err` and the boot continues.

The NVMMU's invalidate FIFO is a different block. iOS's
`FUN_fffffff005eebc9c` polls `+0x22c & BIT(0)` with a hard 20 ms deadline and
**panics** (`"NVMe invalidate FIFO failed to empty"`); Linux's equivalent is
`apple_h9p_nvmmu_invalidate()` (`pcie-apple-h9p.c:1056-1070`) with the same
bit-0 test and a 20 ms timeout, but it returns an error that the completion
path can absorb. (For the record: `0x022c` bit 0 is `EMPTY`, so `0x101`,
`0xa01` and `0xc01` are all "empty"; see §4.4.)

### 2.7 iOS's DART start order, and when it re-runs (verified)

`AppleS5L8960XDART::start` is `FUN_fffffff005fb21f0`; the hardware init is
`FUN_fffffff005fb5b58` (the address in `docs/pcie/enable-freeze.md` is right).
The init body, in order:

```
ldrb  w8, [x0,#0xaa0]        ; REQUIRE "active" (port powered / DART claimed)
bl    0x5fb63e4              ; REQUIRE helper (returns bit0)
ldr   x8, [x19,#0xaf8]       ; error-reflector (64-bit ADT value)
lsr   x8, x8, #12
str   w8, [x9,#0x24]         ;   +0x24 = reflector >> 12
ldr   x8,[x19]; ldr x8,[x8,#0x6b8]; blr x8   ; stream setup (SID -> tables)
ldr   w8, [x19,#0xb04]
str   w8, [x9,#0x0c]         ;   +0x0c = TCR  (0x80 here)
ldr   w8, [x9,#0x0c]
eor   w8, w10, w8
tst   w8, #0x80808080        ; verify every stream byte kept bit 7
b.ne  panic
ldr   w8, [x19,#0xb20]
str   w8, [x9,#0x30]         ;   +0x30 = fetch-config
ldr   w8, [x19,#0xb1c]
str   w8, [x9,#0x20]         ;   +0x20 = diag-config
… perf counters at +0x1004/100c/1010/1018/1000 if enabled …
```

Two properties matter for the warm-boot problem:

1. **Order**: reflector → stream setup → TCR (verified by read-back) → fetch →
   diag. Linux re-applies reflector/fetch/diag in the same order
   (`apple-dart.c:589-600`), but *after* it has already written TTBRs and
   enabled translation in `apple_dart_setup_translation()`
   (`apple-dart.c:747-764`) — a different order from Apple's.
2. **Frequency**: iOS runs this every time the DART becomes active, i.e. on
   every port power-up, because the PCIe DART loses its programming when its
   port cycles (`enable-freeze.md` §"DART: iOS programs registers Linux
   didn't"; round-1 RE `kernelcache-notes.md` step 13,
   `function-dart_force_active`). Linux runs it at probe (`hw_reset`) and once
   more at attach (`attach_config`), and never again — not on a controller
   reset, not on a port re-power.

---

## 3. Decoding this capture

### 3.1 Bit by bit

`err = 0x0500 = 0b0000_0101_0000_0000`

| Field | Value | Meaning |
|---|---|---|
| bit 31 | 0 | **no exception valid → not a fault** |
| bits 6:0 | 0 | no error code at all (not TTBR/STE/PTE/protection/AXI) |
| bit 8 | 1 | the attribute bit that is set in *every* record |
| bit 10 | 1 | **write** |
| bits 25:24 | 0 | stream 0 = the ANS controller's SID |
| `+0x1c` | `0x3bfff0d0` | DART-side `0xbbfff0d0` = admin CQ entry 13 |

Cross-check with the one sample where the direction is independently known:
the r3-hold fault `0x80000102` was the controller **reading** the ASQ, and bit 10
is 0 there. `0x500` is bit 10 set, i.e. a **write**, which is what a completion
write is. **verified** (both the sample and the direction decode).

### 3.2 What bit 8 is — and what it is not

Observed values of `+0x10` across every capture on this Mac:

| Sample | `err` | dir | context |
|---|---|---|---|
| every boot, at `CC.EN` and after the 100 ms watch | `0x100` | read | idle, before any Linux I/O |
| r7-1 after a 64 MiB read (`dmesg-after-read.txt:4951`) | `0x100` | read | `addr 0x3bffc540` (data-queue SQ entry) |
| r7-2 mid-read (`dmesg-after-read.txt:7281`) | `0x500` | write | `addr 0x3bffd0b0` (I/O CQ entry 11) |
| r7-3 (failing), rescue | `0x500` | write | `addr 0x3bfff0d0` (admin CQ entry 13) |
| r3-hold, real STE-invalid fault | `0x80000102` | read | `addr 0x3bffe000` (ASQ) |

Bit 8 is set in **all five**, including a confirmed fault and a confirmed-healthy
idle state, and Apple never tests it. It is therefore not an error code and not
a direction bit; the two consistent readings are "record valid/present" or an
AXI attribute of the recorded access. **inferred** — it cannot be settled from
the driver, and nothing in the diagnosis should depend on it. (This is also why
`0x100` in a healthy boot is not an error: it is the same record with
direction = read.)

### 3.3 The two readings of the register pair

The observations support one of two hardware models, and they differ in what a
"sticky" value means:

- **Model A (latched record).** `+0x10/+0x1c` hold the last *recorded event*
  (a fault, or an access the DART wanted to record). It does not change while
  the DART is idle, which is why `0x100/0x1e0f000` is byte-identical across
  dozens of boots and across acks.
- **Model B (live/last transaction).** `+0x10` bits 8/10 and `+0x1c` describe
  the transaction currently (or most recently) in flight; an idle DART falls
  back to the latched/default `0x100/0x1e0f000`.

The r7-2 pair is the strongest datum and it argues for B:
`0x500 / 0x3bffd0b0` at 33.770411 s (`dmesg-after-read.txt:7281`), then back to
`0x100 / 0x1e0f000` at 34.404327 s (`:7288`) — with the queue still live and,
importantly, **the four TTBRs unchanged** (`0x8080cebc …` in both lines), so
this is not a re-init: the register pair changed by itself. The first dump
caught the I/O queue busy (q1 `12/12`, per `ds-nvme-c-forensics.md` §3.2), the
second caught it idle (q1 `0/0`, `dmesg-after-read.txt:7290`) with the same
IOVAs. That
makes `0x1e0f000` the *idle/default* record rather than an iBoot leftover
(which also explains why every boot prints it), and it means the failing boot's
`0x500 / 0x3bfff0d0` at 50.95 s was a **live** record, not a stale one — the
DART had just recorded a write at the admin CQ. The r7-1 pair
(`0x100 / 0x3bffc540` persisting into the next dump) is the one that argues for
A; it may simply be a longer-lived single record. Either way:

**In both models, bit 31 is the fault gate, and it is clear in every capture of
every warm boot.** The failing boot therefore has no *recorded, valid* DART
fault — at 50.9 s. What it has is a record that the last access the DART
noticed was a **write to admin CQ entry 13**.

### 3.4 The one thing that would hide a fault: Linux's silent ack

There is exactly one way a real fault could have happened and still show as
`0x500`: it was **acked** between the fault and the rescue dump. Linux acks
`+0x10` in three places, and two of them are silent:

| Site | When | Prints? |
|---|---|---|
| `apple_dart_s5l8960x_irq()`, `apple-dart.c:1341` | on every handled fault | yes (the fault line) |
| `apple_dart_s5l8960x_attach_config()`, `apple-dart.c:736` | at every domain attach | **no** (dev_dbg only) |
| `apple_dart_hw_reset()`, `apple-dart.c:627` | at probe and at remove | **no** |

The failing boot *did* re-create its DART domain: the four TTBRs differ between
its own 11.66 s dump and the 50.94 s dump, while both healthy boots keep
identical TTBRs across all four of their dumps (§4.3). A new domain means
`apple_dart_setup_translation()` ran again, which means `attach_config()` ran
again — which W1Cs bits 6:0 and 31 and leaves bits 8/10 and the address.
**verified** (code paths) + **inferred** (that a re-attach happened).

So the honest end state is:

> The rescue dump cannot fully distinguish "the DART translated the write and it
> landed in the wrong place" (§6.1) from "the DART faulted and a later attach
> erased the code and the valid bit" (§6.2). Both leave `0x500` and the address
> behind. The decode alone cannot settle it, **because Linux acked the
> evidence**.

§6.1 is the better fit for the other observations (the register pair's own
return to idle in r7-2, the empty CQ entry, the absence of any fault line), but
the difference is one `dev_warn` — §7.1 — and it is the first thing to land.

---

## 4. Cross-check against the healthy boots

### 4.1 Nothing in the printed DART state drifts

`ds-nvme-c-forensics.md` §3.1/§3.3 already establishes that `tcr`, `diag`,
`reflector`, `fetch`, `linksts`, `cdm`, `irq`, `msi`, `en`, `pmgr`, SART and
NVMMU errors are bit-identical across r7-1, r7-2 and the failing boot, before
`CC.EN` and 100 ms after. I re-checked the three dumps in the photo and agree.
The only moving values are allocator outputs (TCB, scratch, TTBRs, ASQ/ACQ
backing pages).

### 4.2 The early state of the failing boot is normal

With the corrected TTBR digits (§1), the failing boot's `before CC.EN` line is
byte-identical to r7-2's, and its `err 0x100 / addr 0x1e0f000` is the value every
boot shows (documented in `enable-freeze.md` as "error 0x100 (bit 31 'fault'
clear), address 0x1e0f000 … left over from before Linux attached"; §3.3
suggests it is simply the idle form of the record). There is no early warning
sign in the DART state. **verified**.

### 4.3 The one real DART difference: the TTBRs change mid-boot

| Boot | TTBRs at first dump | TTBRs at last dump |
|---|---|---|
| r7-1 | `0x80807d60 0x80807c2c 0x8080cc48 0x80807c3c` | identical (4 dumps) |
| r7-2 | `0x8080cebc 0x8080c8bc 0x80807cb4 0x8080c8ac` | identical (4 dumps) |
| r7-3 | `0x8080cebc 0x8080c8bc 0x80807cb4 0x8080c8ac` | `0x800?cb50 0x800?cbcc 0x000… 0x800?c…` |

**verified** for r7-1/r7-2 and for the *difference* in r7-3; the exact r7-3
rescue digits are not readable.

TTBR encoding, settled: `TTBR = BIT(31) | (PA >> 12)`.
`apple_dart_hw_set_ttbr()` writes exactly that (`apple-dart.c:436-447` with
`ttbr_valid = BIT(31)`, `ttbr_shift = 12`, `addr_field_shift = 0`,
`apple-dart.c:1654-1657`), and the driver's own hardware-view walk decodes it
the same way and then reads the STE/PTE Linux wrote:
`TTBR0 0x80807d60 → STE[0x1df] 0x807ce0003 → PTE[0x1fe] 0x80c4ec003`
(`warm-chain-r7-1/boot01/dmesg.txt:4738`), i.e. PA `0x807d60000`, `0x807ce0000`,
`0x80c4ec000` — all inside `System RAM 0x800000000-0x87b7fffff`
(`cold5b-iomem.txt:42`), and the STE has bits 1:0 = `0b11` exactly as
`io-pgtable-dart` writes them for this DART (`IO_PGTABLE_QUIRK_APPLE_DART_S5L8960X`).
This also disproves the `PA >> 4` reading suggested in
`ds-nvme-c-forensics.md` §3.4: with `>> 4` the walk would have read unrelated
memory and could not have produced the STE/PTE pattern Linux itself wrote.
**verified.**

A new set of TTBRs means a new io-pgtable, which means a new IOMMU domain,
which means the NVMe device was released and re-attached. That is consistent
with the rescue state `state 6 online 0` (`NVME_CTRL_DEAD`,
`drivers/nvme/host/nvme.h:313-321`) and with the queues being `flags 0x0`
(un-enabled, torn down). **verified** for the state value, **inferred** for the
release/re-attach.

### 4.4 Two "anomalies" that are not

- `invalidate FIFO 0x101 → 0xa01`: `0x022c` bit 0 is `EMPTY`; `0xa01` still has
  it set. The differing bits (9, 11) are undocumented status; Linux only ever
  tests bit 0. Not a drain indicator. **verified** (`pcie-apple-h9p.c:246-248`,
  `:926-930`, `:1065-1069`).
- `apple-dart fault handler` IRQ count `0 0`: the same `0 0` appears in both
  healthy boots (`warm-chain-r7-1/boot01/interrupts.txt:3-4`), so a zero count
  is *not* evidence of a masked or mis-routed IRQ. It is the normal value when
  no fault has been delivered. **verified**.

---

## 5. What Linux does today, and where the gaps are

### 5.1 It acks the error register silently (verified)

`drivers/iommu/apple-dart.c:717-737`:

```c
static void apple_dart_s5l8960x_attach_config(struct apple_dart *dart)
{
        u32 reflector = readl(dart->regs + DART_S5L8960X_ERROR_REFLECTOR);
        u32 fetch     = readl(dart->regs + DART_S5L8960X_FETCH_CONFIG);
        u32 diag      = readl(dart->regs + DART_S5L8960X_DIAG_CONFIG);
        u32 error     = readl(dart->regs + DART_S5L8960X_ERROR);
        dev_dbg(…);                       /* not printed with loglevel 7 */
        apple_dart_s5l8960x_hw_config(dart);
        writel(error, dart->regs + DART_S5L8960X_ERROR);   /* W1C, no trace */
}
```

and `apple-dart.c:626-627` in `apple_dart_hw_reset()` does the same at probe.
Consequences:

- A fault between `apple_dart_hw_reset()` (probe, ~0.4 s) and the attach
  (~6.4 s on a warm boot) is latched, then erased by the attach ack, and
  `request_irq()` is not even installed for the first part of that window
  (`apple-dart.c:1583` then `:1587`).
- A fault that is still latched when a domain is re-created (the failing boot)
  is erased the same way.

This is the reason the rescue dump cannot say whether the DART faulted.

### 5.2 The reset path cannot flush the TLB (verified)

`apple-dart.c:602-642`:

```c
apple_dart_hw_disable_dma(&stream_map);          /* TCR[sid] = 0 */
apple_dart_hw_clear_all_ttbrs(&stream_map);
…
writel(readl(dart->regs + dart->hw->error), dart->regs + dart->hw->error);
if (dart->hw->type == DART_S5L8960X) {
        apple_dart_s5l8960x_hw_config(dart);
        return 0;                                /* <-- no invalidate_tlb() */
}
return dart->hw->invalidate_tlb(&stream_map);
```

The comment above the early return explains why: this DART only honours the
invalidate command with translation enabled. That is true and matches iOS — but
it leaves a warm boot with **stale translations and no flush between the reset
and the first attach**. On a cold boot the TLB is empty, so nobody ever noticed.

### 5.3 The attach-time flush is unchecked (verified)

`apple_dart_setup_translation()` (`apple-dart.c:739-765`) writes TTBRs, re-applies
the globals, enables translation, then calls
`stream_map->dart->hw->invalidate_tlb(stream_map)` and ignores the result. On
failure the only output is one `dev_err` from
`apple_dart_s5l8960x_hw_stream_command()` (`apple-dart.c:488-493`), and the boot
continues with whatever was in the TLB.

### 5.4 The order differs from Apple's, and the state is never re-established (verified)

| Step | iOS (`FUN_fffffff005fb5b58`) | Linux at attach (`apple-dart.c:739-765`) |
|---|---|---|
| reflector `+0x24` | 1st | after TTBR writes |
| stream setup | 2nd | TTBR writes (equivalent) |
| TCR `+0x0c` + read-back check | 3rd, verified | 4th, **not verified** |
| fetch `+0x30` | 4th | after |
| diag `+0x20` | 5th | after |
| invalidate | (on activation) | last, **unchecked** |

And nothing re-runs it after a controller reset: `nvme_pci_apple_ans_reset_dma()`
(`drivers/nvme/host/pci-apple-ans.c:429-443`) rebuilds only the NVMMU
(TCB/scratch/INIT/SART), and `nvme_pci_apple_ans_preinit()` (`:492-566`) stops
the controller and calls it. Neither touches the DART.

---

## 6. The chain, ranked

The chain in the round-7 write-up — *warm boot → the DART faults on the
completion write → I/O stalls* — is **not supported**: there is no bit 31, no
error code and no IRQ in any capture of the failing boot. But the *address* is
real and it is exactly the completion the driver was missing. These are the
chains that fit that pair of facts, best first.

### 6.1 H1 (most likely): the write succeeded into a stale translation

1. Boot N−1 hands over a DART whose TLB still holds translations, including
   DVA `0x3bfff000` (the CQ, an IOVA every boot uses:
   `warm-chain-r7-1/boot01/dmesg-after-read.txt:4939-4953` vs the photo).
2. Boot N probes the DART. `apple_dart_hw_reset()` clears TCR and the TTBRs
   but **returns before `invalidate_tlb()`** (`apple-dart.c:636-639`, §5.2), so
   the stale entry is still there.
3. At attach, `apple_dart_setup_translation()` writes the new TTBRs, enables
   translation, and issues **one** invalidate whose result is discarded
   (`apple-dart.c:764`, §5.3). If that command does not take effect — busy bit
   stuck, issued in the wrong window, or simply not covering the entry — the
   stale translation survives into the running system.
4. The controller writes the completion for the command that was in flight. The
   DART hits the stale entry and writes into **boot N−1's** CQ page, which boot
   N has since reallocated to something else. The translation *succeeded*, so
   there is nothing to report: no code, no bit 31, no IRQ. The DART's
   last-transaction record still names the DVA it was given
   (`0x3bfff0d0`), which is what the rescue dump shows.
5. The driver's CQ never gets the entry — the snapshot at 50.9 s shows the head
   entry empty (`next status 0x0 cid 0x0`) — so the request times out, the
   controller is reset once, then declared `NVME_CTRL_DEAD`, the device is
   released, the IOMMU domain is destroyed and re-created (new io-pgtable, new
   TTBRs, §4.3), and the initramfs rescues.

This explains every observation at once: `0x500` (a write), no fault bits, the
exact address of the missing completion, the empty CQ entry, the TTBR change,
the absence of any early warning, and the boot-count dependence (it only bites
when the backing page for `0xbbfff000` moves between boots — the queue pages are
freshly allocated every boot, and ds-nvme-c §3.4 shows the ASQ/ACQ PA moving in
every boot of the epoch).
**Inferred**, from verified pieces. The falsifier is §7.1: if the next failure
prints a `+0x10` with bit 31 set, or if a healthy boot shows the flush timing
out, the model is wrong.

### 6.2 H2: a real fault that was acked before the dump

Same start, but the stale entry is *invalid* rather than stale: the write misses
the tables, the DART records a PTE/TTBR-invalid fault with bit 31, the IRQ
either does not fire or fires after the driver has already given up, and a later
attach (`apple_dart_s5l8960x_attach_config()`, §5.1) acks the code and the valid
bit, leaving only bits 8/10 and the address.

Weaker than H1 because r7-2 shows the register pair returning to its idle form
by itself (`0x500/0x3bffd0b0` → `0x100/0x1e0f000`, §3.3), so an acked record
would have to survive to 50.9 s; and because a fault would normally reach the
handler, which is installed and shared, and would have printed a line.

### 6.3 H3: no DART involvement at all

The register pair is a live last-transaction trace, the stall is entirely
controller/firmware-side (a completion the controller never issued), and the
DART merely recorded the last write it did perform — possibly a much earlier
one. Cannot be excluded, but it explains less: the address lands exactly on the
one entry the driver was missing, which is a strong coincidence.

**What is *not* on the list:** a translation/protection fault whose code we can
read. There is no such code in any capture, on any boot.

---

## 7. The change

Three pieces, in the order they should land. (a) is a few lines and makes the
next capture conclusive; (b) is the actual hardening; (c) are two follow-ups.
All of them are DART-owned code — the ANS glue only needs to call the new entry
point at controller start.

### 7.1 Log before ack (do this first)

Add two fields (`last_acked_err`, `last_acked_addr`) to `struct apple_dart`,
then in `apple_dart_s5l8960x_attach_config()` and in `apple_dart_hw_reset()`:

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Rationale: it changes no hardware behaviour, it prints once per distinct value,
and it would have told us in one line whether the failing boot's DART faulted
and on which stream. Keep the ack (removing it would make the next boot's
`0x100` look like a fault forever).

### 7.2 Re-establish the DART at every controller start, in Apple's order

Add one exported entry point in `apple-dart.c` (or a small iommu notifier) and
call it from `nvme_pci_apple_ans_preinit()` right after the controller is
confirmed stopped (`pci-apple-ans.c:556`) and before
`nvme_pci_apple_ans_reset_dma()`. It must run with the controller at `CC=0`,
`CSTS.RDY=0`, i.e. with no DMA in flight. This is the sequence that makes
`§6.1` impossible: it *forces* every inherited translation out, instead of
trusting one unchecked command to do it.

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Notes on why each step exists:

- **Step 1** is new. `apple_dart_hw_reset()` does it at probe only, and it cannot
  flush afterwards (`§5.2`); doing the quiesce here, at controller start,
  guarantees the `TCR=0` window happens with the controller stopped rather than
  at probe time, and it drops any TTBR the previous kernel left.
- **Step 2** reproduces `FUN_fffffff005fb5b58`'s order (reflector first). Today
  `apple_dart_setup_translation()` applies the reflector *after* the TTBRs
  (`apple-dart.c:747-760`), which is harmless in practice but is not the
  sequence Apple validates against.
- **Step 4** is Apple's own post-write check; Linux has none today.
- **Step 5** is the core fix for §6.1: a stale entry can only survive a
  `TCR=0` + TTBR-clear + re-enable + flush if the flush silently fails, and this
  makes that failure fatal. It is the same policy Round 7 already adopted for
  the NVMMU ("a failed stop blocks later endpoint access").
- **Step 6** closes the "wrote it but the DART dropped it" case, which is real
  for this block: the registers read all-ones while the port is powered down
  (`enable-freeze.md`).

Keep the existing `hw_reset()`/`attach_config()` behaviour otherwise: they are
correct as a boot-time path, and the new entry point makes them idempotent
rather than load-bearing.

### 7.3 Two smaller items

- Make `apple_dart_setup_translation()` and `apple_dart_domain_flush_tlb()`
  propagate the `invalidate_tlb()` result instead of discarding it
  (`apple-dart.c:656`, `:764`), and `dev_err` once per failure.
- Print the DART state (`err`, `addr`, TTBRs, TCR) in
  `apple_ans_host_dump()` **before** any code path that can ack it, and take the
  dump at the *first* timeout, not only at rescue. Round 7 already has the
  hooks; they just run after the state has been rewritten.

---

## 8. What to capture next time (cheap, high value)

1. **Text, not a photo.** The failing boot's only record is a JPEG; two digits
   are unrecoverable and one transcription digit is wrong. A `pstore`/ramoops
   record or an ACM capture of the ANS lines would have settled §3.3 outright.
   Round 7 already reserves ramoops and installs `/linux-a9-ans-state`; the gap
   is that the rescue shell had no working console.
2. The **first** `host` dump (at the first timeout), before the automatic reset,
   not just the post-reset rescue dump.
3. The four TTBR registers **with the early values in the same line**, so a
   change is unambiguous even from a photo.
4. `dmesg | grep apple-dart` in the rescue shell — one line per acked state
   after §7.1 lands.
5. On the healthy side: one boot with `nvme.apple_ans_debug=1` plus a
   `echo host > apple_ans_diag` before and after a known 64 MiB read, to pin
   down Model A vs Model B for `+0x10/+0x1c` (§3.3). This is a one-boot
   experiment and it removes the last ambiguity in the decode.

---

## 9. Corrections to earlier notes

| Claim | Correction |
|---|---|
| "`err 0x500` … a likely DART fault on the controller's completion write" (`2026-10-23-round7-warm3-rescue.md:58-62`) | Not a fault: bit 31 (valid) and all code bits are clear; Apple's handler calls this spurious. The address arithmetic is right. |
| "Both apple-dart fault IRQs show count 0, so the fault was never delivered" (same, `:62`) | Count 0 is also the healthy value (`interrupts.txt:3-4`); it carries no information. |
| `0x8000cebc …` for the failing boot's early TTBRs (`:30`, and `ds-nvme-c-forensics.md` §3.4) | The photo reads `0x8080cebc`, identical to N=2. One `8` was lost in transcription. |
| "TTBR is consistent with a `PA >> 4` encoding" (`ds-nvme-c-forensics.md` §3.4) | It is `BIT(31) \| (PA >> 12)`: `apple-dart.c:436-447`, `:1654-1657`, and the hardware-view walk resolves to the exact STE/PTE Linux wrote. |
| "`invalidate FIFO 0x101 → 0xa01`" as a symptom (`2026-10-23-round7-warm3-rescue.md:63`) | `0x022c` bit 0 is `EMPTY` and is set in both; the rest is undocumented status. |
| "`err 0x100` … left over from before Linux attached, origin unknown" (`enable-freeze.md`) | Consistent with the decode, and probably not a leftover: it is the *read*-direction form of the record with no error code and no valid bit, and r7-2 shows the register returning to it once the DART goes idle (`dmesg-after-read.txt:7288`). It survives acks because bit 8 is not W1C. |

---

## 10. Open questions

1. **Is bit 8 "record valid" or an AXI attribute?** Only a hardware experiment
   (inject a known protection fault, read `+0x10` before and after the ack)
   settles it. It does not change any conclusion here.
2. **Model A vs Model B** for `+0x10/+0x1c` (§3.3) — one healthy boot with two
   dumps around a known read, per §8.5.
3. **Did the failing boot's DART fault before the re-attach?** Answered by §7.1
   on the next failure, and only there.
4. **Where does the completion write actually land?** Under H1 it lands in a
   reallocated page; under H2 it is reflected. The `error-reflector`
   (`0x20fffc000`) is not inside RAM (`0x800000000+`); if it ever fires, the
   write goes to a window Linux neither reserves nor maps. Worth one read of the
   ADT to see whether iOS reserves it as scratch — if it does not, the reflector
   is a hang risk on its own.
