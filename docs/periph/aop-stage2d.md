# AOP stage 2d–2h and fixed chained stage 3d

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

2026-10-25. Implementation task, host/Docker only. No device access, workspace
commit or push. Kernel commits are limited to `a9/aop` and `a9/tri49a`. The
owner accepts crash risk and authorizes bounded, logged AOP control writes.
No syscfg, NVRAM, boot-storage, firmware-image, remap, reset or PMGR write is
introduced. Auto-boot below is a four-byte **AON MMIO control**, not boot storage.

## What today's captures establish

The [s2b registers](../release/OMISSIONS.md) show inbox
`4008=00000101` even before this attempt. Every one of its six TX words adds
`0x100`, ending at `00000701`. EMPTY bit17 stays clear, the outbox remains
`00020001`, and RX=0. `0b88` tracks the inbox exactly; `0044` stays zero.
The mask writes change `0c00/0c04` from `0x1111` to `0x110`. The `4000/4004`
readbacks are zero throughout, so use the mirror to judge remaining mask bits.

**This is a delivery/servicing failure before management decoding.** A
running mailbox receiver would need to dequeue even an unsupported HELLO
reply. A protocol-version mismatch, runtime type2 dispatch or missing Linux
RX IRQ cannot by itself explain accumulated, unconsumed inbox words. Ordinary
type6 wake messages already queued in that same FIFO cannot fix a receiver
which never dequeues them, unless their arrival independently wakes hardware.
The count-field interpretation is inferred from its one-for-one TX increments;
the documented full/empty/fault bits and the raw evidence are retained.

**Actual CPU execution remains unproved.** iOS's `isCPURunning` tests only
`+b88 & 1`; its V2 read helper at `fffffff005b4e9b4` is a direct MMIO load.
There is no PC sample, heartbeat or firmware reply here. The identical inbox
and b88 evolution is particularly poor independent execution evidence. The
survivor may be in WFI with a masked interrupt, stuck with internal IRQs
disabled, or parked in a different boot-chain program. No observed register
distinguishes those cases. `cpu44=0` means the iOS AP run-request bit is absent;
it does not prove halt or establish what another boot stage did. The earlier
`auto_boot=0` was an unread placeholder and remains unusable evidence.

## iOS evidence and ranking

All addresses are unslid in the pinned 20H350 j71t kernelcache, SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
Reproduce retained instruction windows and Ghidra results with:

```sh
python3 -B scripts/periph/aop/re-stage2d.py --ghidra
```

1. **IOP-side mailbox masks, ranked first.** iOS `b4e548/b4e5a8` clear 1
   and `0x1000` at both `+4004` and `+c04`, corresponding to AP inbox-space
   and outbox-not-empty sources. These were already tried. The untested
   complement is `0x110`: bit4/bit8 as IOP inbox-not-empty/outbox-space is a
   **layout hypothesis**, not an established A9 hardware result. Clearing
   only those remaining bits is the narrowest test of the non-draining FIFO.
   A second entry isolates bit4 alone. No mask-all/reset/source ACK is used.
2. **Run request.** `setCPURun(1)` at `b4e7c8` conditionally RMWs `+44`
   bit4=1, preserving the other bits, then writes 1 to mapped auto-boot
   when that property exists. The saved live ADT has `auto-boot` and the
   four-byte reg at `0x210000600`; it has no `cpu-ctrl-filtered` property.
   Linux entries isolate the run bit from the full two-store release. These
   are real iOS stores, but static code cannot establish their effect on this
   unidentified survivor. They never first clear the run bit.
3. **Combined masks and run/auto-boot.** Useful if neither alone succeeds;
   four stores total. Type6 `0x220` follows through the normal FIFO sequence.

**Critical correction: `+c004=0x11; +c024=1` is an NMI trigger, not normal
mailbox wake.** `b4e830` is V2 vtable slot `+6b8`. The provider action at
`b4ba58` accepts option1 only while its AP `_started` flag is set and invokes
that slot. RTBuddy `_signalNmi`'s tail at `b67040` calls this provider action
after a type15 management request; its `b7e36c` path prints `%s NMI` and panics.
The `_signalNmi` string at `fffffff005314c41` corroborates the caller identity.
Ghidra's original function boundary starts in a tail, so instructions and
caller/string evidence matter more than the reconstructed C prototype.
The shared analysis is annotated, and that caller is named
`RTBuddy_signalNmi_tail`. **No entry reads/writes the c000 NMI block or sends
type15.** An NMI-induced firmware crash would not establish mailbox recovery.

`AppleA7IOP::startCPUWithOptions` at `b4b738` normally sets AP mask/outbox
state and invokes `setCPURun(1)`; its option-bit0 warm path skips the remap and
CPU-release stores. The already-started rejection uses an AP software boolean,
not b88. RTBuddy's `b65b54` warm transition sends type6/0x220 after provider
start. There is no demonstrated extra ordinary AP TX doorbell in the FIFO
accessor: `b4e4a8` is a single 64-bit store to `+4010`. AFK `0xa2` doorbells
are application transport after management, not a substitute for IOP mailbox
interrupt delivery. No complete iOS cold start is replayed over unknown code.

## Fixed stage-3 pool

The [s3b iomem capture](../release/OMISSIONS.md) contains:

```text
858000000-8583fffff : reserved
  858000000-8583fffff : reserved
```

The DT and phandle were correct. `of_reserved_mem_lookup`/validation succeeded:
otherwise the old function returned `-EINVAL`, not `-EBUSY`. arm64
`request_standard_resources()` inserts the no-map outer resource;
`reserve_memblock_reserved_regions()` adds a busy generic child for the
persistent FDT header reservation. `request_mem_region()` descends only
through **nonbusy** conflicts, so the second generic reservation rejected it.

The fix preserves both reservations and calls `devm_request_resource()` for
an exclusive driver child under exact, same-size `reserved` ancestors. It
rejects other owners, System RAM, a different extent, partial overlaps and
unexpected depth. A second claimant fails. The same fixed 4 MiB pool is then
mapped Normal non-cacheable, with the existing bounded AFK bump allocator.
No ordinary DMA allocation or late memblock reservation is substituted.
Published buffers remain reserved even after handshake/AFK failure.

## Entries and bounds

One kernel, immutable `apple_a9_aop.wake`, with `attach_stage=2` or 3 and all
other AOP modes off. `cycle=1`, invalid values, and nonstaged use are rejected
before allocation/mapping. `wake=0` preserves the old b/c ladder.

* **s2d / wake=1:** `+4004=0x110`, `+c04=0x110`. First diagnostic choice.
* **s3d / wake=1:** the same attempt, then AFK/accel in that **same boot** if
  HELLO/EPMAP completes. No prior stage-2 success or second handshake required.
* **s2e / wake=2:** `+4004=0x10`, `+c04=0x10`, inbox-source isolation.
* **s2f / wake=3:** `+44 |= 0x10` only.
* **s2g / wake=4:** `+44 |= 0x10`, then `*(0x210000600)=1`.
* **s2h / wake=5:** d's two clears, then g's two CPU-release stores.

Each observes passively for 5 s first, so an immediate valid handshake can
avoid hardware stores entirely. Otherwise one hardware action (1–4 stores),
observe 1 s, ping0 (0.5 s), type6/0x220 (2 s), HELLO_REPLY12 (2 s), AP-awake
type0xb/0x20 (1 s), HELLO_REPLY12 (2 s). Silent bound: **13.5 s of receive
windows, five TX words, at most four hardware stores**, plus bounded TX polls
(20 ms per send), scheduling and log overhead. No 0x201 sleep or retry ladder.
All stores have a pre-store log, ordered readback and saved snapshots on both
sides. A mailbox fault aborts remaining stores. The shared atomic attempt guard
blocks a second attempt even after failure; debugfs only reads frozen RAM.
Protocol replies can add bounded TX; the 512-word startup budget remains.
Successful stage 3 continues the existing bounded startup and streaming path.

Snapshots retain masks, +44, b88, inbox/outbox/idle and a validity-tagged
auto-boot value. Only g/h map/read/write auto-boot; it remains explicitly
invalid elsewhere. No Linux RX IRQ is enabled. Polling is the sole consumer.

## Orchestrator commands (not executed here)

Keep the recovery/default entry and root server process intact. Select one
entry per boot, retain the complete capture even if it crashes or stays silent.

```sh
source build/periph/aop-stage2d/entries.env

# First choice: fixed pool + first-ranked wake, then AFK in the same boot.
ENTRY="$AOP_S3D_ENTRY" scripts/boot/kexec-host.sh "$AOP_S3D_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage3d

# Diagnostic-only equivalent; choose instead of the command above.
ENTRY="$AOP_S2D_ENTRY" scripts/boot/kexec-host.sh "$AOP_S2D_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage2d

# Independent alternatives; select one after reviewing the previous capture.
ENTRY="$AOP_S2E_ENTRY" scripts/boot/kexec-host.sh "$AOP_S2E_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage2e
ENTRY="$AOP_S2F_ENTRY" scripts/boot/kexec-host.sh "$AOP_S2F_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage2f
ENTRY="$AOP_S2G_ENTRY" scripts/boot/kexec-host.sh "$AOP_S2G_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage2g
ENTRY="$AOP_S2H_ENTRY" scripts/boot/kexec-host.sh "$AOP_S2H_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage2h
```

**Readouts:** c00/c04 should lose selected bits relative to `step-before`
(fresh 0x1111 may become 0x1001; inherited 0x110 may become 0). A count drop
or EMPTY transition after clearing them supports hardware IRQ gating; any RX
or ping ACK establishes receiver liveness. No EPMAP can still mean a live
runtime firmware not entering its handshake. `+44` readback gaining bit4 or
auto-boot reading 1 establishes register acceptance only. A self-clearing bit
or unchanged value is recorded, never retried. Neither identifies a running
image. Frozen backlog after all relevant hardware writes keeps internal IRQ
masking, parked/stuck/foreign code and other internal gates unresolved.

For s3d, first look for `stage3 reserved AFK pool`, a nested
`a9-aop-stage3-pool` in iomem, and a `wake=1` attempt in debugfs. This alone
proves the probe bug is passed. The pool can be claimed even if the handshake
then fails. Sampling additionally requires HELLO/EPMAP, application AFK init,
all new grants inside `0x858000000..0x8583fffff`, accel service/id, first raw
report and `phase=streaming`/IIO. Capture samples again after tilting. A wake
success does not prove old DMA references were released or establish image
identity. Kexec does not reset AOP: compare each boot's initial registers and
backlog; a later variant can inherit changes from an earlier one.

## Reproduction and handoff

```sh
python3 -B scripts/periph/aop/test-offline.py
bash scripts/periph/aop/build-stage2d.sh
python3 -B scripts/periph/aop/package-stage2d.py
python3 -B scripts/periph/aop/tests/stage23-artifacts.py
python3 -B scripts/periph/aop/tests/stage2d-package.py
```

> Private graphics-dependent excerpt omitted from this source snapshot.

Kernel implementation commit: `cd353c8438822028b43ffeedf7c0024c0d800eb0`
on `a9/aop`; integration commit: `a3712df6e658d0258f91d26c97139e69f3e8b186`
on `a9/tri49a`. Both worktrees are clean. Full tri48a-overlay SHA256:
`27e987b03367e52aa69d293c416c1c6392f7407b41087fc98398feb80681eff6`.
Release: `7.3.0-rc1-tri49a-s2d-27e987b033-ga3712df6e658`.
Names are `tri49a-s2d-27e987b033`, `tri49a-s3d-27e987b033`, and
`tri49a-s2{e,f,g,h}-27e987b033`; every cmdline is 981 bytes. Payloads and
entry directories are under `build/periph/aop-stage2d/`; use `entries.env`.

> Private graphics-dependent excerpt omitted from this source snapshot.

Task-owned workspace files, all left **uncommitted**:

```text
docs/periph/aop-stage2d.md
docs/periph/aop-stage2b.md
docs/periph/aop-stage23.md
docs/periph/ds-aop-attach.md
scripts/periph/aop/build-stage2d.sh
scripts/periph/aop/package-stage2d.py
scripts/periph/aop/re-stage2d.py
scripts/periph/aop/check-build.py
scripts/periph/aop/package-stage23.py
scripts/periph/aop/test-offline.py
scripts/periph/aop/kernel/a9-aop-experiment.h
scripts/periph/aop/kernel/apple-a9-aop-active.c
scripts/periph/aop/kernel/apple-a9-aop.rst
scripts/periph/aop/tests/experiment.c
scripts/periph/aop/tests/mailbox.c
scripts/periph/aop/tests/pool.c
scripts/periph/aop/tests/stage2d-package.py
```

The three kernel files changed on each branch are
`Documentation/driver-api/apple-a9-aop.rst`,
`drivers/soc/apple/a9-aop-experiment.h` and
`drivers/soc/apple/apple-a9-aop-active.c`. The pre-existing canonical binding
edit (`apple,j71t-aop.yaml`) and unrelated workspace work were retained.
