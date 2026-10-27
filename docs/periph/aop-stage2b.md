# AOP stage 2b/2c and chained stage 3b/3c

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25 correction: **`+0xb88` is not verified CPU status.** It mirrors
inbox `+0x4008` in every saved boot today, including FIFO occupancy. Earlier
running/halted conclusions from its bit0 are withdrawn; execution remains
unknown. The current forced-cold path accepts RUN `+0x44` bit4 clear after
10 ms, with auto-boot/outbox disabled and fault-free mailboxes, under the
owner's accepted crash risk. See [aop-force-cold.md](aop-force-cold.md).
Historical captures and earlier path descriptions below retain their provenance.


Next implementation: [stage 2d–2h and fixed chained stage 3d](aop-stage2d.md),
using today's non-draining inbox result and fixing the s3b reserved-pool claim.

2026-10-25. Implementation sub-agent, host/Docker only. Owner accepts crash
risk; no firmware-identity or old-DMA proof gate. No device access, workspace
commit, push, AON power/reset write, or syscfg/NVRAM/boot-storage access.

## Findings

**The old result does not establish why the AOP is silent.** It establishes
one successful AP store attempt and no observed RX during that window. The
next entries separate FIFO delivery, interrupt-mask state, ordinary wake and
a management sleep/wake transition. Executing-image identity is still unknown.

Evidence below is static inspection of the pinned 20H350 kernelcache and
firmware, or the owner's saved stage-2 capture; no new hardware result is claimed.

* **HELLO encoding is correct for the pinned firmware.** Endpoint 0 is bits
  63:56; management type is 55:52. iOS `handleHello` at
  `fffffff005b7196c` accepts a range containing 12 and replies
  `00200000000c000c`. Firmware `010311f6` requires min=max=12. This is a
  HELLO **reply**, not an AP-originated HELLO. The normal first word is IOP
  `00100000000c000c`. An RTKit release/build number is not the wire version.
  Blindly sweeping versions or moving the endpoint to an ASC msg1 is unsupported.
* **The previous re-enumeration conclusion was too broad.** Type 2 resets the
  EPMAP cursor in the handshake receiver `010311ac`; that does not mean every
  firmware state dispatches type 2 there. The separate runtime switch at
  `0103131c`, table `01031336`, routes type 2 to `01031378`, which signals
  a generic event (`01033cf8`) rather than calling the EPMAP emitter
  `01031538`. A fully booted peer need not answer an unsolicited reply.
  This is a plausible explanation for silence, conditional on image/state.
* **Direction, offsets and width match iOS.** `fffffff005b4e498` consumes one
  64-bit word at wrapper `+4038`; `fffffff005b4e4a8` stores one 64-bit word
  at `+4010`. No separate doorbell occurs in that FIFO accessor. Controls are
  `+4008` / `+4020`; empty/full are bits 17/16 and faults bits 19:18.
  The old driver did not read back the controls after TX. Its `tx_words=1`
  proves the write path ran, not that firmware consumed the word.
* **Zero IRQs were expected in that experiment.** It never requests/enables
  the Linux RX IRQ, and polls the FIFO. No IRQ cannot explain an empty FIFO
  throughout polling. Hardware masks are a separate question: iOS enable
  helpers `fffffff005b4e548` / `...e5a8` write 1 / 0x1000 to BOTH `+4004`
  and `+c04`; disable selects `+4000` / `+c00`. The new ladder reproduces
  those four enable writes, with a reply window before sending another word.
  It does not mask all other sources or enable an unhandled Linux IRQ.
* **`0x220` and `0x201` are different transitions.** iOS's state table at
  `fffffff0053110a0`, sender `...5b71750`, contains type-6 words
  `201/220/202/0` for target states 0/1/2/3. Its boot path
  `...5b65b54` sends `0060000000000220` when coming from status 3.
  Firmware decoder `010314f4` maps low-byte 0x20 to internal 0x21, whereas
  0x201 maps to internal 8. Thus 0x201 is not a second wake encoding.
* **Why the optional cycle is useful:** runtime type 6 calls decoder
  `010314f4` then state request `01032ed0`. That setter wakes its event only
  when the requested state changes. Transition callback `01031054` has a
  path leaving run state 0x21 that resets management handshake fields and
  disables application endpoints. Its return-to-0x21 path can emit a fresh
  v12 HELLO (`010310c4..010310dc`) and wait for map completion. This supports
  testing one 201→220 pair after the non-sleep ladder fails. It does not prove
  all power policies take that path, that mailbox wake works from state 8,
  or that AFK buffer ownership is cleared. The code does not clear AFK tables.
* **CPU control is unresolved.** iOS's `setCPURun` at `...5b4e7c8` RMWs
  `+44` bit 4 and optionally writes auto-boot; `isCPURunning` at `...5b4e4b8`
  tests `+b88` bit 0. `cpu_control=0` is not enough to identify halt/WFI or
  the running program. No CPU-release experiment is included under the hard
  limits. The old experiment's `auto_boot=0` was a zero-initialized placeholder:
  it never mapped/read that resource. New state explicitly says
  `auto_boot_valid=0`.

Pinned SHA256 identities are unchanged from [the attach study](ds-aop-attach.md).
`python3 -B scripts/periph/aop/re-stage2b.py` retains instruction windows in
`build/periph/aop-stage2b/re/`. It uses Capstone for tiny Ghidra tail-shared
accessors and checks both artifact hashes. The shared Ghidra kernelcache was
also queried; its wake sender is now named `RTBuddyManagement_sendWake220`,
and both interrupt helpers have comments. Expanded private firmware Ghidra
results are under `build/periph/aop-attach/firmware/`. Some decompilations
merge switch/data/panic tails; the cited instructions, not inferred C function
boundaries, support the transition claims.

## Bounded order

All entries use the same kernel. Stages 2 and 3 retain their existing selector;
the c variants additionally set immutable `apple_a9_aop.cycle=1`.

1. `passive`, 5 s: poll; answer a real HELLO and process its map.
2. `hello12`, 2 s: one speculative `00200000000c000c`.
3. `mailbox-unmask`, 1 s: four writes (1, then 0x1000, each to +4004/+c04).
4. `ping0`, 0.5 s: `0030000000000000`. ACK is liveness, never handshake proof.
5. `wake220`, 2 s: `0060000000000220`.
6. `hello-after-wake`, 2 s: v12 reply.
7. `ap-awake-b20`, 1 s: `00b0000000000020`, the iOS AP-awake notification.
8. `hello-after-ap`, 2 s: v12 reply.
9. **c only:** `quiesce201`, 0.5 s: `0060000000000201`.
10. **c only:** `rewake220`, 2 s: `0060000000000220`.
11. **c only:** `hello-after-cycle`, 2 s: v12 reply.

The silent-case windows total 15.5 s (b) / 20 s (c), plus bounded TX polling
and logging overhead. No response means at most 6 / 9 TX words and four
mask writes. A valid IOP HELLO can be answered after any speculative reply;
up to three real HELLOs are accepted before map processing. Ping/power ACKs
before HELLO are logged and tolerated. A partial map, invalid version/order,
fault, full-FIFO timeout or startup-transcript exhaustion ends the experiment.
There is no automatic reboot, second ladder run, cold load or reset fallback.

A complete EPMAP immediately stops later alternatives, sends map ACKs and AP
readiness, then records 2 s more replies. `winner` names that observation
window, not proven causation: earlier words can have delayed effects. Stage 2
freezes with zero grants. **Stage 3 proceeds directly from that exchange in
the same boot** to the existing reserved AFK/IIO path. It does not redo HELLO
or require a preceding stage-2 success. A separate stage-2 boot can change the
survivor's state, so repeating b in a later kexec need not reproduce its winner.

The fixed 4 MiB pool and persistent FDT reservation are unchanged. New grants
stay inside it; inherited DMA remains unknown. System endpoints are logged
and not started. Stage 3 still needs actual AFK initialization, an accel
announcement and a first report; re-enumeration alone does not create them.

## Registers and interpretation

`registers` is a frozen RAM transcript, also printed as `REG` lines to dmesg.
Each snapshot includes +44, +b88, +4000/+4004, +c00/+c04, +4008, +4020 and
+4040. These are status/control reads; FIFO data is never read for a dump.
Every actual data transfer remains in `startup_transcript` / `transcript`.
Snapshots bracket each step and each mask write. TX has `tx-before`, ordered
`tx-after0`, and `tx-after50us` (sleep minimum 50 µs; use recorded timestamps).
Wait windows sample nominal 1/10/100/1000/2000/5000 ms and their endpoints.
There are 256 retained register snapshots; `registers_dropped` reports
saturation. Streaming does not generate more register snapshots. Debugfs reads
perform no MMIO and cannot trigger another attempt.

* Inbox EMPTY clears immediately, then sets: evidence that a FIFO word queued
  and was consumed. Raw low bits are retained; their counter definition is
  not known and is not asserted.
* Inbox stays nonempty/full: delivery/servicing problem. Look for a change
  specifically across unmask or wake. A full send times out within 20 ms.
* Inbox stays empty even at `tx-after0`: inconclusive; a fast consumer can win
  the readback race. It does not prove a dropped store or a wrong offset.
* Outbox becomes nonempty/RX starts after unmask: supports mask dependence.
  `irq_enabled=0` remains intentional because the sole reader is the poller.
* Ping/power ACKs but no EPMAP: transport/firmware liveness without handshake.
  In c, look for a type-7 ACK reporting 0x201, then a real type-1 HELLO after
  `rewake220`, followed by EPMAP and readiness. No ACK does not skip rewake.
* `failed-ladder-no-epmap` with RX=0 after all steps still leaves image
  identity, halted/stuck firmware, internal interrupt gating and mailbox
  servicing unresolved. Do not label it a version mismatch from silence.
* Stage 3: `phase=streaming`, grants inside 0x858000000..0x8583fffff,
  `accel`/0x10000007 announcement, increasing `reports`, changing axes in
  `samples`/IIO. Keep failure transcripts even if only management worked.

## Build and orchestrator commands

Host-only reproduction (no firmware-identity/old-DMA proof gate):

```sh
python3 -B scripts/periph/aop/test-offline.py
bash scripts/periph/aop/build-stage2b.sh
python3 -B scripts/periph/aop/package-stage2b.py
python3 -B scripts/periph/aop/tests/stage23-artifacts.py
```

> Private graphics-dependent excerpt omitted from this source snapshot.

Orchestrator only; these were **not run** by this task. Keep recovery/default
entry and the root-server process intact. No identity or old-DMA proof approval
is required before choosing an entry.

```sh
source build/periph/aop-stage2b/entries.env
ENTRY="$AOP_S2B_ENTRY" scripts/boot/kexec-host.sh "$AOP_S2B_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage2b

# Optional explicit sleep/wake fallback, or select this directly:
ENTRY="$AOP_S2C_ENTRY" scripts/boot/kexec-host.sh "$AOP_S2C_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage2c

# Sampling entries run the matching ladder and chain AFK in the SAME boot.
# Choose b or c; there is no prerequisite to run the diagnostic-only entry.
ENTRY="$AOP_S3B_ENTRY" scripts/boot/kexec-host.sh "$AOP_S3B_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage3b
# Or:
ENTRY="$AOP_S3C_ENTRY" scripts/boot/kexec-host.sh "$AOP_S3C_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage3c
```

The collector waits for the boot attempt and reads `registers` as well as the
existing state/transcripts/rings/samples. Its default capture directory remains
`build/periph/aop-stage23/<label>-<UTC time>/`; set `AOP_CAPTURE_DIR` to override.

## Commits, validation and workspace handoff

The previously uncommitted overlays were committed first: `a3b151757f` on
`a9/aop`, `37f12544e8` on `a9/tri49a`. New implementation commit on `a9/aop`:
`0db1e7ac27`. Final integration/artifact details are appended below after build.

Offline tests execute the actual management/ladder/grant functions and the
actual TX/status/unmask functions with fake clocks/FIFOs, under ASan/UBSan.
They cover late HELLO, early ACKs, b/c silent bounds, c re-wake success,
partial maps, invalid versions, send failures, immediate versus delayed FIFO
consumption, full/fault behavior and mask-only MMIO writes. Hardware behavior
is unverified. Existing reservations and payload checks are structural checks,
not a claim that the surviving firmware released all old DMA.

Final integration commit: `d62aab2ce1cad78fb641b3d9acf5686c5c916013` on
`a9/tri49a`. Both kernel worktrees are clean. The full overlay SHA256 is
`fa8030e0516aeb1ff7eb9ba8464ef17743404c3bef28aca6184729b70a055f2d`.
Release: `7.3.0-rc1-tri49a-s2b-fa8030e051-gd62aab2ce1ca`.

Packaged entry names are `tri49a-s2b-fa8030e051`,
`tri49a-s3b-fa8030e051`, `tri49a-s2c-fa8030e051`, and
`tri49a-s3c-fa8030e051`. The b command lines are 988 bytes; c is 982 bytes.
All four entries and DFU payloads are under `build/periph/aop-stage2b/`;
`entries.env` names them and `manifest.json` records their hashes and the
clean implementation commit. The kernel build is in `build/kernel-tri49a/`.

> Private graphics-dependent excerpt omitted from this source snapshot.

Task-owned workspace files, all left **uncommitted**:

```text
docs/periph/aop-stage2b.md
docs/periph/aop-stage23.md
docs/periph/ds-aop-attach.md
scripts/periph/aop/build-stage2b.sh
scripts/periph/aop/package-stage2b.py
scripts/periph/aop/re-stage2b.py
scripts/periph/aop/capture-stage23.sh
scripts/periph/aop/firmware-attach-re.py
scripts/periph/aop/kernel/a9-aop-experiment.h
scripts/periph/aop/kernel/apple-a9-aop-active.c
scripts/periph/aop/kernel/apple-a9-aop.rst
scripts/periph/aop/package-stage23.py
scripts/periph/aop/test-offline.py
scripts/periph/aop/tests/experiment.c
scripts/periph/aop/tests/mailbox.c
```

> Private graphics-dependent excerpt omitted from this source snapshot.

