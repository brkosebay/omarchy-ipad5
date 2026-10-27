# Forced AOP cold restart, tri49a

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**cold2b follow-up (v3):** [aop-cold-hello.md](aop-cold-hello.md) audits the
silent v2 release, documents the retained-wake candidate and frozen SRAM/FIFO
diagnostics, and gives the current commits/entries/test commands. The body
below records the preceding v2 build (`3ac4bbc15c` / `d17671c36b`); its 5 s
HELLO bound and artifact hashes are historical. No successful cold boot has
been established offline.

2026-10-25. Implemented and built **offline**. No device connection, workspace
commit or push. Kernel commits only on `a9/aop` and `a9/tri49a`. The owner
accepts crashes and authorizes the bounded control writes below. Syscfg,
NVRAM and boot storage are untouched.

The [stage 2d–2h / stage 3d captures](aop-stage2d.md) leave the inherited inbox
FULL, with no dequeue or RX despite mask/run/auto-boot writes. This experiment
selects a new stop/load path, bypassing the running-attach ladder entirely.
Both entries are immutable opt-ins: `aop_force_cold=1` plus
`apple_a9_aop.attach_stage=2` (cold HELLO/EPMAP only) or 3 (same cold boot,
then AFK/accel). All other AOP modes, wake and cycle must be off.

## cold2 result and corrected register interpretation

The [original cold2 capture](../release/OMISSIONS.md) ran the earlier commits
`44d0f4d26e` / `71ba8c4d4c`. All five stop writes landed: outbox bit0 clear,
mask words 0x1111, RUN bit4 clear, auto-boot zero. It then refused with
`COLD halt errno=-110 status=00010801`, without popping or loading anything.
All **11** frozen snapshots have `+b88 == +4008`, including after RUN clears.
Earlier boots show both words evolving together through 0x20001, 0x501,
0x601, 0x701 and 0x10801 as AP TX words accumulate. This is strong evidence
of an inbox status mirror/alias; the hardware decoding itself is unverified.
The AOP's execution under Linux has **never been established** by these bits.
The initial RUN=0 observation is consistent with a core that never ran.

The pinned V2 implementation supplies no independent reliable halt predicate:

- `fffffff005b4e378` identifies `AppleA7IOPV2`; allocator `b4e3ac` installs
  vtable **fffffff006d3ff98**. This confirms the class, rather than trusting
  previous manually assigned Ghidra function names.
- Vtable **+680 → b4e4b8** (`isCPURunning`) passes **0xb88** to **+640 →
  b4e9b4**, a direct `ldr w0,[wrapper,w1,uxtw]`, then returns bit0. There is
  no second base, translation or independent CPU status read in that path.
- **+6b0 → b4e7c8** (`setCPURun`) changes only **+44 bit4**, then writes the
  mapped AON auto-boot word. This is command state, not a PC status register.
- **+6d8 → b4c460** returns success without reading hardware; **+6a8 →
  b4e7c4** is RET. The V2 diagnostic helper **b4e624** reads idle +4040 and
  mailbox words, but defines no idle/halt bit. Stop **b4b8fc** never waits for
  the b88 predicate after clearing RUN.

Use the owner's authorized fallback: **RUN=0 after a fixed 10 ms delay**,
with auto-boot/outbox-enable zero and no mailbox faults. This accepts a stop
command; **PC halt and DMA quiescence remain unknown**. `+b88` remains a raw
snapshot only and cannot block drain or forced load. Stage 1 now emits
`wrapper_b88=... execution=unknown`, and an enabled empty mailbox result is
`observed-mailbox-enabled-empty`; the old running/halted verdicts are withdrawn.
The two-word state probe similarly reports `powered execution=unknown`.
Other non-forced active modes retain their legacy b88 admission heuristic;
that heuristic is not execution evidence and is bypassed by forced cold.

## Sequence and evidence

Wrapper base is **0x210800000**. The four-byte **AON MMIO** auto-boot control is
**0x210000600**, not boot storage. Sources are unslid addresses in the pinned
iOS 16.7.10 / 20H350 j71t kernelcache, SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.

1. Check all five PMGR domains through the existing read-only guard. Refuse a
   mailbox fault. Acquire the shared atomic one-attempt guard before any store.
   Do not send type6/0x201: the inherited inbox cannot accept or service it.
2. Disable outbox: `+4020 = read(+4020) & ~1`. This is
   `setOutboxEnable(0)`, **b4e5d8**, called by `AppleA7IOP::stop`, **b4b8fc**
   (call site b4b948). Linux RX IRQ has never been requested for either entry.
3. Mask mailbox sources: `+4000=0x1111`, `+c00=0x1111`. This uses the iOS
   mask-all helper **b4e530** (called by cold start **b4b738**), as additional
   isolation during the forced stop. iOS stop itself disables its two AP
   interrupt-event sources through **b4bb4c**; it does not call mask-all there.
4. Clear RUN: `+44 = read(+44) & ~0x10`; then `auto-boot=0`.
   Both are `setCPURun(0)`, **b4e7c8**, called at **b4b9c8** from stop.
   Preserve all unrelated CPU-control bits.
5. Wait **10 ms** after the last stop write, then require RUN bit4,
   auto-boot and outbox enable bit0 to read zero, with no mailbox faults.
   Log `COLD halt errno=0 predicate=run-clear-delay cpu44=... rawb88=...
   delay_ms=10 execution=unknown`. A stuck control or fault ends the attempt
   with `failed-force-halt`, before FIFO pops, load, remap or release.
6. With that gate satisfied, discard at most **64 words per FIFO**. Read
   inbox data at **+4018** and outbox data at **+4038**, testing EMPTY bit17
   and fault bits18/19 before every pop. Recheck RUN/auto-boot/outbox-enable
   zero before each pop and after each FIFO. Never dispatch stale words or
   clear fault bits. Require both queues EMPTY afterwards; otherwise
   `failed-force-drain`, with no load. Log every discarded word and both
   surrounding status snapshots.
7. Request and SHA256-check the two pinned firmware files. Reserve ext in the
   retained pool at **0x858000000**, 16 KiB aligned; the 37,024-byte ext uses
   **0xc000** bytes. Prepare the existing tag patches: AOPt, nCal, EC0p,
   WrAd, McRA, SOC_, SOCR, DEVf, STKG, PRNG, RTTO. Leave absent/default
   NONC/ECID alone. This is the existing cold loader and iOS RTBuddy segment
   path **b88090/b88238**, tag logic **b70290**, and load **b6f340/b6f9b4**.
8. Recheck domains, RUN/auto-boot/outbox-enable zero and fault-free
   empty FIFOs after firmware lookup. Ignore b88, even if bit0 remains set. Only then stage ext and copy/read-verify
   the patched main image at **0x210e00000**, extent **0x9b000**. No BSS clear.
   A static no-map node and persistent FDT header reservation protect this
   target. The AFK pool has both protections too.
9. Replay `mapFirmware`, **b4e72c**, in order:
   `+18=0`, `+20=0`, `+28=0x9b000`, `+30=0`, `+08=0x10e00000`,
   `+10=2`, `+38=1`.
10. Replay cold start **b4b738**: `+4000=0x1111`, `+c00=0x1111`,
    `+4004=1`, `+c04=1`, `+4004=0x1000`, `+c04=0x1000`, then
    `+4020 |= 1`, `+44 |= 0x10`, `auto-boot=1` (**b4e7c8**).
    Both forced entries use polling; no Linux RX IRQ is enabled.
11. Wait for a **device-originated HELLO**, reply version12, process/ack EPMAP,
    send normal AP readiness. The iOS handlers are **b7196c**, **b71afc** and
    **b716d0**. No speculative HELLO, ping/wake ladder or second handshake.
    Stage2 ends with `cold-stage2-handshake-complete`, before STARTEP/AFK.
12. Stage3 continues in this same boot: normal application STARTEP, AFK v8
    initialization, bounded buffers from the remainder of the reserved pool,
    discover `accel`/ID `0x10000007`, saved calibration, and batch/raw/report
    **100000/0/100000**. Register `apple-a9-aop-accel` IIO only after the first
    valid report. Raw bytes are exposed in debugfs `samples`, `rings` and
    `state`. AFK/calibration/rate/report sources remain those in
    [ds-aop-attach.md](ds-aop-attach.md).

Every control/remap store logs `COLD-W` with bank, offset, old and written
values, then `COLD-R` with an ordered readback. Frozen `registers` snapshots
cover both sides. Firmware image RAM is word-copied and read-verified; it is
not a register and is not logged per word. Ordinary mailbox TX keeps its
existing wire transcript and pre/post status snapshots. Trace storage is
bounded (640 register snapshots, 512 startup words, 128 raw samples).

Other bounds: firmware lookup 180 s; HELLO 5 s; EPMAP 2 s; stage3 IOP-ready
2 s (existing driver records missing 0x20 and continues); service 5 s;
property replies 2 s each; first sample 3 s; each FIFO TX-space poll 20 ms.
After a forced-path failure, debugfs reads cannot trigger a retry. Stage2
stops immediately after EPMAP/readiness transmission, without a ping loop.

## What remains experimental

**No separate AOP core-reset register has been established.** The ADT supplies
no reset line and has empty gate arrays. The V2 vtable slot +6a8 is the
unmap hook **b4e7c4**, a RET; it is not an asserted core reset. Its idle hook
**b4c460** is a success stub. We replay the available vendor stop/start controls,
without a PMGR cycle, NMI or guessed reset bit. Clearing/releasing RUN has not
been demonstrated to restart at the firmware entry point.

**The accepted halt predicate is a delayed command readback.** There is no
independent verified run/halt register or DMA drain indication. The old b88
gate was invalidated by cold2; it is removed from stop, per-pop checks and
the pre-copy revalidation in this forced path.

**Inbox +4018 is an explicitly inferred, untested AP-side pop.** iOS verifies
send at +4010 and outbox pop at +4038 (**b4e4a8/b4e498**). The pinned firmware's
generic mailbox initialization **0102ce1c..0102cf22** defines the first queue's
status/send/receive offsets **8/10/18**, and receiver **0102cdd8** loads its
64-bit receive half. This motivates +4018 for the AP inbox, but no iOS AP
inbox-drain call or AP bus-access permission has been verified. A fault, stuck
queue or bus trap is possible. Crash risk is owner-accepted; no hardware
success is claimed. Firmware-input identity is pinned; post-release execution
identity and old DMA quiescence still require device evidence.

Reproduce the pinned instruction windows without connecting to hardware:

```sh
python3 -B scripts/periph/aop/re-force-cold.py
```

Output: `build/periph/aop-force-cold/re/`, including stop/start/masks/remap,
V2 class identity/vtable/direct accessor, vendor predicate, diagnostic helper,
idle stub, firmware FIFO layout/receiver, and `halt-evidence.json` summarizing
the 11 equal b88/inbox snapshots. Ghidra comments record the correction; the
misnamed V1 run/outbox helpers were renamed to V2 after class verification.

## Orchestrator commands — not executed by this implementation agent

Keep the recovery/default entry and existing root-server process intact.
Select one entry per boot, then save its complete capture.

```sh
source build/periph/aop-force-cold/entries.env

# First diagnostic: stop/load, device HELLO/EPMAP, no application start.
ENTRY="$AOP_COLD2_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=210 scripts/periph/aop/capture-stage23.sh cold2-runreadback

# After reviewing cold2, this independent boot chains AFK and accel.
ENTRY="$AOP_COLD3_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD3_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=210 scripts/periph/aop/capture-stage23.sh cold3-runreadback

# After tilting the iPad, collect another read-only sample snapshot.
scripts/periph/aop/capture-stage23.sh cold3-runreadback-tilted
```

The capture includes state, frozen registers/startup transcript, raw
samples/rings, IIO x/y/z/scale/frequency, runtime DT/iomem and full dmesg.
No new attempt is started by capture. On a crash or timeout, retain the host
log/console/pstore before the ordinary DFU recovery; do not clear the evidence.

Look first for five `cold-stop` writes, then
`COLD halt errno=0 predicate=run-clear-delay ... delay_ms=10 execution=unknown`.
`rawb88=00010801` is allowed here. Require `COLD stale fifo=inbox index=0..7`
for the inherited eight words, decreasing inbox occupancy and both EMPTY
bits afterwards. `stale_inbox=8` is expected for the reported inherited state;
other counts must agree with their initial snapshot. A full stuck inbox
ends after 64 pops with `failed-force-drain`; faults/changed controls stop
sooner. Those failures must have no `cold-load` or `AOP released` lines.

Then require remap readbacks, `AOP released`, **RX HELLO** and EPMAP, ending
with `cold-stage2-handshake-complete`. `halt_accepted=1` only records the
accepted command predicate. The debugfs `snapshot` is the initial guarded
snapshot; `halt_control` and frozen `registers` show post-stop readbacks.
Neither acceptance nor release proves firmware execution. `failed-no-hello`
after release means the control sequence completed without protocol evidence.
Save the complete console/pstore if the inferred inbox pop causes a trap.

For cold3, require all published ext/AFK PAs inside
`0x858000000..0x8583fffff`, AFK ring descriptors, accel service/id, successful
calibration/rate replies, first raw report, `phase=streaming`, `iio=1`, and
changing raw axes after tilt. IIO scale/cadence retain their existing working
interpretations; a successful property ACK alone does not validate them.

## Build, artifacts and changed files

```sh
python3 -B scripts/periph/aop/test-offline.py
bash scripts/periph/aop/build-force-cold.sh
python3 -B scripts/periph/aop/package-force-cold.py
python3 -B scripts/periph/aop/tests/force-cold-package.py
```

> Private graphics-dependent excerpt omitted from this source snapshot.

Task-owned workspace files changed this follow-up (all **uncommitted**):

```text
docs/periph/aop-force-cold.md
docs/periph/aop-attach-test-plan.md
docs/periph/aop-state-probe-test-plan.md
docs/periph/aop-stage2b.md
docs/periph/aop-stage2d.md
docs/periph/aop-stage23.md
docs/periph/aop-sensors.md
docs/periph/ds-aop-accel.md
docs/periph/ds-aop-attach.md
docs/periph/ds-aop-standalone.md
docs/periph/ds-aop-takeover.md
scripts/periph/aop/build-force-cold.sh
scripts/periph/aop/re-force-cold.py
scripts/periph/aop/check-build.py
scripts/periph/aop/tests/force-cold.c
scripts/periph/aop/tests/force-cold.py
scripts/periph/aop/tests/state-probe.c
scripts/periph/aop/kernel/apple-a9-aop-active.c
scripts/periph/aop/kernel/apple-a9-aop.c
scripts/periph/aop/kernel/apple-a9-aop.rst
scripts/periph/aop/kernel/apple,j71t-aop-observer.yaml
```

Four changed kernel paths on each allowed branch:

```text
drivers/soc/apple/apple-a9-aop-active.c
drivers/soc/apple/apple-a9-aop.c
Documentation/driver-api/apple-a9-aop.rst
Documentation/devicetree/bindings/soc/apple/apple,j71t-aop-observer.yaml
```

The four paths above contain the follow-up changes; the three existing
experiment-header/DT paths remain allowlisted and unchanged. Pre-existing
`apple,j71t-aop.yaml` edits and unrelated workspace work were preserved.
No workspace commit or push was made. The historical cold2 capture was not edited.

Build provenance and validation for the revised artifacts are recorded below.

Current branch tips: **`a9/aop` = 3ac4bbc15c02e3e7b78b0ac272f40baeef0468f4**; **`a9/tri49a` = d17671c36b04027f94ef9bf9fe0b878ecb7524c8**.
These retain original commits `44d0f4d26e` / `71ba8c4d4c` as ancestors.
Each adds the halt/observer correction, a separate binding description commit,
and the final per-pop status-fault check. Both worktrees are clean.

Release: `7.3.0-rc1-tri49a-cold-24c7d65c52-gd17671c36b04`.
Full tri48a-overlay SHA256: `24c7d65c522b62d0f031852c82d00e5cafbd5db07ff3cd081e0d48ed1d4c4281`.
Entries: **`tri49a-cold2-24c7d65c52`** and **`tri49a-cold3-24c7d65c52`**, each **980 bytes** of cmdline.

The payloads and extracted warm entries, `entries.env`, firmware hashes,
commit/config metadata and full file hashes are in
`build/periph/aop-force-cold/manifest.json`. Its
`hardware_execution_verified=false` remains explicit. No new device run was made.

> Private graphics-dependent excerpt omitted from this source snapshot.

Actual payload extraction reproduces Image/DT/initramfs/cmdline. Single-gzip
firmware/calibration contents, preserved baseline archive prefix, both persistent
reservations through the real warm/recovery DT merger, all manifest/entry
SHA256 hashes and shell syntax pass.

Logs under `build/periph/aop-force-cold/`: `offline-run-readback.log`,
`cold-run-readback.log`, `aop-w1.log`, `tri49a-w1.log`,
`build-run-readback.log`, `checkpatch-run-readback.log`,
`package-run-readback.log`, `package-test-run-readback.log`, and
`ghidra-save-run-readback.log`. The pinned disassembly and summary are in `re/`.
