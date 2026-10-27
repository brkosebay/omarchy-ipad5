# Cold2b: silent release, retained-wake candidate and SRAM evidence

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25. Host/Docker/Ghidra only; **no device access, workspace commit or
push**. Owner accepts bounded AOP writes and crash risk. Syscfg, NVRAM and
boot storage are untouched. This follows the cold2 v2 implementation at
`a9/aop 3ac4bbc15c` / `a9/tri49a d17671c36b` described in
[aop-force-cold.md](aop-force-cold.md).

**The offline evidence does not establish why cold2b missed HELLO, or prove a
hardware fix.** No map/length/tag error was found in the checked loader. The
new entries test one plausible retained-sleep cause with the documented
RTBuddy wake word, give the boot 20 seconds, and retain the missing execution
evidence. Do not report the candidate as a successful cold boot until tested.

## What cold2b establishes

[The capture](../release/OMISSIONS.md) shows the delayed RUN-clear predicate
passed, eight stale inbox words were popped, main/ext were loaded and the
21 stop/load/release control writes completed. No HELLO or other RX word was
seen in five seconds. RUN command readback is not a PC or DMA-halt predicate.

At the final auto-boot store, the secondary mask `+c00` changes `0x110 -> 0`
and idle `+4040` changes `0x79 -> 0x68`. This is post-release hardware
activity, **not proof that the new image executed**: register side effects or
old firmware are not excluded. There is no saved SRAM baseline/current pair,
PC, or identified crash buffer in that capture. `+b88` continues mirroring
inbox status. The old firmware may have been stopped, resumed, or replaced;
the capture cannot distinguish those states.

## Checks against the pinned iOS and firmware instructions

All iOS addresses below are unslid in the j71t 20H350 kernelcache, SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
Firmware addresses are **linked AOP VAs, not AP physical addresses**. Main
SHA256 is `af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8`.

1. **Remap/window: checked, no correction justified.** V2 `mapFirmware`
   `fffffff005b4e72c` writes descriptor length `& ~0xfff` to `+28`, literal
   PA low32 to `+08`, PA>>32 to `+10`, zero lower bound `+18/+20`, zero `+30`,
   enable 1 at `+38`. The current values are `0x9b000`, `0x10e00000`, `2`.
   These are not page-number encodings. The mapped descriptor is the whole
   reserved firmware target; `RTSZ=0x6e000` is the required main runtime
   capacity, not a replacement descriptor length. The separate ext is not
   added to `+28`. V2's class/vtable identity remains verified as in v2.

2. **Ext placement: software path checked; live DMA accessibility unresolved.**
   SPU `b88090` allocates page-aligned DRAM, copies ext and flushes it;
   `b88238` writes the buffer's **full physical address** into `EC0p`.
   `EC0v`/`EC0s` do not exist in the pinned table, so the generic iOS writes
   for those tags are no-ops. Firmware `0102849e..010284b0` imports EC0p into
   object `01047510+8`. Mapping loop `01030a2a..01030a54` forms a physical page
   number from **both halves**, `(lo>>12)|(hi<<20)`, and maps it at the linked
   ext span beginning `0106e000`. It does not truncate the 0x8 PA high word.
   Thus `0x858000000`, 16 KiB aligned and reserved through the warm merger,
   meets this software contract. There is no observed DART/SART programming
   in the checked j71t path and no AOP mapper in its saved DT. That is not a
   hardware proof that every AP DRAM page is reachable. Ext now receives an
   AP readback comparison before release; actual AOP reads still need testing.

3. **Tag table: checked.** Packed 35-tag table `+389b8`, length `0x1df`;
   numeric tags are little-endian u32 values, with little-endian payloads.
   The loader finds/patches existing tags, preserving order and sizes. Its
   AOPt=8, nCal=0, EC0p=64-bit PA, WrAd=`0x210800000`, McRA=`0x4000`, SOC_,
   SOCR, DEVf, STKG, PRNG and RTTO patches match the previously traced iOS
   operations. NONC/ECID retain defaults without the optional chosen chip ID.
   No new tag/header patch is introduced. The random STKG tag and runtime
   global at `+47474` provide a useful discriminator: C entry `01027412`
   invokes `01000b5c` to copy STKG into that global. Before execution the
   runtime global is the image default `0x00000aff`.

4. **Entry/reset vector: checked within start hooks.** The file's first ARM
   instruction branches to `+68`; that position-independent stub sets VBAR,
   page tables and eventually enters linked Thumb `01027390`. Neither V2
   `mapFirmware` nor the checked `start/setCPURun` hooks writes a separate
   entry-point register. Linked `0x01000000` is not a new remap base to guess.

5. **Core reset: unresolved hardware behavior.** V2 unmap `b4e7c4` is RET;
   run `b4e7c8` changes `+44 bit4` and AON `0x210000600`. Start's prepare
   `b4b68c -> b4b630` invokes the power/clock helpers whose j71t gate arrays
   are empty; its mapper hook is `b4b6f4`. V2 `+6b8 -> b4e830` writes mailbox
   doorbells `+c004/+c024`, not an established core reset. No independent
   reset pulse was found in this bounded search. No reset bit, power cycle,
   NMI or reset-vector store is added. Whether RUN 0->1 restarts at the vector
   remains a device question; the new STKG/page-table/BSS snapshots address it.

6. **IRQ masks: iOS sequence checked.** AP startup masks `0x1111` in both
   blocks and clears bits0/12 via `+4004/+c04`, leaving `0x110`. The outbox
   notification bit12 is unmasked. Both entries use polling with no Linux
   RX IRQ, so an AP interrupt delivery problem alone cannot explain no RX.
   IOP-side mask/runtime behavior is not independently established by AP
   readback. The new build preserves the checked iOS mask sequence.

7. **HELLO FIFO: vendor offsets checked; alternate delivery unresolved.** V2
   receive `b4e498` performs a 64-bit read at `+4038`; outbox-empty helper
   `b4e578` tests bit17 of `+4020`. Firmware `010310ce..010310dc` sends HELLO
   `0x00100000000c000c`. Nothing found justifies changing the Linux RX FIFO.
   New frames also freeze status at the firmware's old `+b88/+ba0` aliases,
   alongside `+4008/+4020`. They do not read another data register or pop a
   second alias speculatively. A nonempty alias with an empty canonical FIFO
   would be evidence for a follow-up, not automatic permission to drain it.

Reproduce this evidence without hardware:

```sh
python3 -B scripts/periph/aop/re-force-cold.py
```

`build/periph/aop-force-cold/re/` now also holds ext-tag import/page mapping,
HELLO send, guard init, prepare/mapper and wake instruction windows, plus
`cold-hello-evidence.json` with the exact pinned table and cold2b transitions.
Shared Ghidra comments on mapFirmware and the wake condition are updated.

## Candidate change and bounded diagnostics

RTBuddy `b65cd0..b65cdc` sends `0x0060000000000220` through `b71fc4` **only if
its saved old status is 3**. Ordinary new cold boot has no such unconditional
requirement. Forced Linux bypasses the protocol power transition and has no
verified saved status; a retained sleeping instance is plausible. The new
candidate therefore polls passively first, then sends exactly one documented
wake after 1 second without HELLO. It re-drains the normal outbox after the
SRAM scan before sending, so a HELLO arriving during the scan suppresses wake.
No speculative HELLO/reply, sleep command, retry or second boot is sent.
A failed wake TX is terminal and reports `failed-cold-wake-tx`.

The first HELLO is polled synchronously, independent of AP IRQs/background
work. The total wait deadline is 20 seconds from release, matching iOS's
default boot-state timeout; this does not claim that a healthy HELLO normally
takes that long. Once HELLO arrives the existing management/EPMAP flow runs;
cold2 still stops before STARTEP, cold3 continues AFK/accel in the same boot.

Before release, read the entire reserved `0x210e00000..0x210e9afff` aperture
using aligned u32 reads under the existing five-domain PMGR guard. Keep that
baseline and the latest successful after-copy in private vmalloc RAM. No
diagnostic pointer is shared with AOP. Capture at release, 10 ms, 100 ms,
pre/post wake, 5 s, and HELLO/deadline: **at most eight frames including the
baseline**, fewer on early HELLO or error. Samples are non-atomic. A failed
power guard skips MMIO; a fault/capture error terminates the attempt.

Each frame logs `COLD-M` whole-aperture SHA256, runtime guard/STKG, and changed
word counts/first old/new value in boot, text/tags, writable data, BSS and
scratch. `COLD-F` logs both FIFO views, masks and idle. `cold_memory` contains
all frozen frames and the remap/control words. `cold_before`/`cold_after` are
the complete binary copies. Reading any of these files after failure performs
no hardware access and cannot start another attempt. The collector saves
them and produces `cold-memory-report.txt`, including bounded changed-text
candidates. No RTKit crash/log layout is claimed; raw bytes preserve one if
present, without following untrusted firmware pointers.

## Orchestrator commands — not executed here

New commits, both clean, preserving the user's v2 commits as ancestors:

- `a9/aop`: `d7b35952b4253ba9add102868bd57aae3e342065`
- `a9/tri49a`: `639dd79ce201005bb5d3521c593dd4a088484d0e`

Release `7.3.0-rc1-tri49a-cold-c663659545-g639dd79ce201`.
Full tri48a-overlay SHA256:
`c6636595453e8f8e2acdd332664d5ce4144b2b86b93411487ad382be53fc729f`.
Entries **tri49a-cold2-c663659545** / **tri49a-cold3-c663659545**, 980-byte
cmdlines. Manifest, entries.env and private pinned firmware/calibration remain
under gitignored `build/periph/aop-force-cold/`.

Keep recovery/default and the existing root server intact. Run cold2 first;
review its entire capture before selecting cold3 on a separate boot.

```sh
source build/periph/aop-force-cold/entries.env
ENTRY="$AOP_COLD2_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold2-hello-v3

# After reviewing cold2:
ENTRY="$AOP_COLD3_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD3_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold3-hello-v3

# After tilting, collect cached samples/IIO without starting another attempt:
scripts/periph/aop/capture-stage23.sh cold3-hello-v3-tilted

# Optional host-only reinspection of a capture directory:
python3 -B scripts/periph/aop/inspect-cold-memory.py /path/to/capture
```

Read the results as follows:

- HELLO before wake: `wake220_sent=0`; early capture/longer observation or
  altered scheduling helped. It does not validate the retained-wake hypothesis.
- HELLO after one successful wake, then EPMAP: candidate works on that boot;
  the timing supports retained sleep, but does not prove causation. Preserve
  the pre-wake memory/status frame and first RX word.
- No HELLO, guard changes from default to patched STKG with expected page-table
  and BSS writes: supports fresh execution reaching C entry. Inspect scratch,
  writable globals and changed-text candidates for a crash or later init stall,
  including ext access. No SRAM change: execution is still unproved, with
  retained PC/reset/start mapping the next unresolved branch. Do not call that
  a proved stopped CPU. Unexpected text changes merit separate investigation.
- Empty `+4020` but nonempty `+ba0`: retain all status frames and raw bytes;
  investigate FIFO routing before another data-register read.
- Success is RX HELLO/EPMAP and `cold-stage2-handshake-complete`; for cold3,
  additionally require reserved-pool grants/rings, accel/calibration/rate ACKs,
  actual raw reports, `phase=streaming`, IIO and changing axes after tilt.
  Release logs, RUN, masks, or memory activity alone are not success.

## Validation and changed files

> Private graphics-dependent excerpt omitted from this source snapshot.

Logs: `build-hello.log`, `aop-w1.log`, `tri49a-w1.log`,
`checkpatch-content-hello.log`, `offline-hello-final.log`, `package-hello.log`,
`package-test-hello.log` under `build/periph/aop-force-cold/`.
Hardware execution remains unverified in the manifest.

This task's workspace edits, all **uncommitted**:

```text
docs/periph/aop-cold-hello.md                         (new)
docs/periph/aop-force-cold.md
docs/periph/ds-aop-standalone.md
scripts/periph/aop/build-force-cold.sh
scripts/periph/aop/capture-stage23.sh
scripts/periph/aop/inspect-cold-memory.py             (new)
scripts/periph/aop/re-force-cold.py
scripts/periph/aop/kernel/apple-a9-aop-active.c
scripts/periph/aop/kernel/a9-aop-memory.h
scripts/periph/aop/kernel/apple-a9-aop.rst
scripts/periph/aop/tests/force-cold.c
scripts/periph/aop/tests/force-cold.py
scripts/periph/aop/tests/cold-memory.c                (new)
scripts/periph/aop/tests/memory.c
```

> Private graphics-dependent excerpt omitted from this source snapshot.

Follow-up: [cold2 v3 cached SRAM and cache/IRQ isolation](aop-cold-stall.md)
maps the new execution evidence and supplies the next entries. The v3 device
run confirms loaded-code execution; its backing globals are cache-incoherent.
