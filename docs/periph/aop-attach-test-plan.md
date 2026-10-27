# Staged AOP attach — offline validation and orchestrator handoff

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Superseded for stages 2/3 by the owner-authorized experimental build and commands
in [aop-stage23.md](aop-stage23.md). Stage 1 below is unchanged.

2026-10-25. Research: [ds-aop-attach.md](ds-aop-attach.md). No device commands
were run by this task. No commit or push. Kernel base is `a9/aop`
`906da12f51cbd53bf8bc13df196281983f3fe5de`; changes remain uncommitted in
`/src/wt/aop`. Canonical templates live in `scripts/periph/aop/kernel/`.

## What is ready to test

| Selector | Actual implementation | Device decision |
| --- | --- | --- |
| `attach_stage=1` | One PMGR-guarded, frozen status snapshot. Four u32 wrapper reads, no FIFO consume or writes. | **Safe diagnostic candidate** on the identified j71t with the existing recovery workflow; same powered-MMIO qualification as state_probe. |
| `attach_stage=2` | Early `refused-no-verified-host-epoch`; no maps, PMGR reads, mailbox or DMA. | Safe **refusal regression** only. A real HELLO/handshake experiment is **not authorized by the evidence in this handoff**. |
| `attach_stage=3` | Same early refusal. | Safe **refusal regression** only. No AFK buffer or accelerometer-enable attempt. |

Stage1 cannot reveal the payload of a pending mailbox word: reading +0x4038
would consume it. Pending status is retained untouched. The existing Round4
`active=1 attach=1` experiment is separate and must not substitute for stage2.

## Host validation

These commands are host/Docker-only and operate only on `a9/aop`:

```sh
python3 -B scripts/periph/aop/capture-attach-re.py
~/.local/share/uv/tools/ghidra-mcp/bin/python -B scripts/periph/aop/firmware-attach-re.py
python3 -B scripts/periph/aop/test-offline.py
scripts/build-kernel.sh --run aop '
python3 -B /work/scripts/periph/aop/install-kernel.py
make -j4 LLVM=1 ARCH=arm64 W=1 drivers/soc/apple/apple-a9-aop.o drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-ring-reader.o
scripts/checkpatch.pl --no-tree --strict --file drivers/soc/apple/apple-a9-aop.c
git diff --check
'
KERNEL_JOBS=4 scripts/build-kernel.sh --tree aop
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aop'
```

The installer verifies `/src/wt/aop` and branch `a9/aop`. It must refuse other
trees. No commit is needed to inspect or compile the change. The full build
exports `COMMIT`, `DIRTY`, `SOURCE.patch`, config and Image.gz/DTBs under
`build/kernel-aop/`. **COMMIT is the base**, not a new implementation commit;
pair it with `SOURCE.patch` and `DIRTY`. An additional kernel diff is retained
in `build/periph/aop-attach/kernel.patch`. Do not publish a clean-build claim
from the base commit alone.

The host harness compiles the **actual** PMGR guard, observation callback,
capture and debugfs show functions with ASan/UBSan and a fake MMIO backend.
It verifies:

* All five domain locks/read checks precede exactly four status reads.
  CPU control, FIFO data, interrupt masks, reset/power and firmware memory
  are absent from the observer's accessible resources/calls in staged mode.
* Running/empty, pending/unconsumed, stopped, disabled outbox and both fault
  bits on both mailboxes; hardware words remain unchanged.
* Every domain OFF/transition/reset/disable/parent-off condition, software
  genpd OFF, regmap errors, provider deferral, and balanced references/locks.
* Repeated debugfs reads use only the frozen string. Stages2/3 with **null
  mappings and domain pointers** still refuse with zero hardware reads.
* Immutable stage selector, invalid values, mode conflicts, postcore ring-capture
  exclusion and early active
  exclusion are checked in the source/build validation. The linked image
  contains the parameter and matches canonical templates and J71t-only DT.

Existing protocol/ring/calibration/takeover/parser suites also run. They
validate software and old policy helpers; their success does not prove the
old speculative takeover's DMA-ownership assumption.

Validation outputs for this task are in `build/periph/aop-attach/`:
`offline.log`, `w1.log`, `build.log`, `check-build.log`; shared RE in `re/`,
extracted-kext fallback in `spu-local/`, pinned receiver analysis in `firmware/`.
No device acceptance, DMA-drain proof, rate or six-pose validation is claimed.

Results: production-function ASan/UBSan suites **PASS**; W=1 AOP object
compilation **PASS**; full arm64 Image.gz + **104 DTBs PASS**; linked symbols,
parameters, templates and DT checks **PASS**. Strict observer checkpatch:
**0 errors, 0 warnings, 0 checks**. The build is intentionally dirty against
the base commit; no new implementation commit exists.

## Stage1 device procedure — orchestrator only, not run here

1. Review the canonical source/diff and host results. Integrate the changes
   through the normal full Omarchy/NVMe/kexec kernel workflow, preserving the
   recovery entry, m1n1/PSCI configuration, root transport and all non-AOP
   baseline arguments. The standalone `aop` image is **not** that integrated
   boot image. Do not modify the integration branch from this research task.
2. Use the existing observer DT; no new resource, reserved memory or firmware
   file is needed. `CONFIG_APPLE_A9_AOP=y`, `DEBUG_FS=y` suffice for stage1.
   ACTIVE may remain compiled in to test the exclusion, but use active=0.
3. Remove **all prior `apple_a9_aop.*` tokens** from the selected test cmdline,
   then add the following. Preserve every non-AOP token. Omit ring_pa and
   ring_pa_size entirely.

```text
apple_a9_aop.attach_stage=1 apple_a9_aop.debug=0 apple_a9_aop.state_probe=0 apple_a9_aop.attach=0 apple_a9_aop.active=0 apple_a9_aop.ring_scan=0 apple_a9_aop.ring_iio=0
```

4. Package a distinct test entry using the established integration/payload
   scripts. Record integration COMMIT, source patch hash when dirty, Image.gz
   hash, DTB hash and the complete cmdline. A warm boot is adequate for this
   observation; another power-off experiment does not supply an epoch reset.
5. Boot once through the established orchestrator workflow. After normal USB
   SSH readiness, collect the **saved** result. The following commands are
   instructions for the orchestrator, not commands executed by this task:

```sh
mkdir -p build/periph/aop-attach/capture-stage1
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline; sudo -n dmesg' \
  > build/periph/aop-attach/capture-stage1/identity.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/attach_status' \
  > build/periph/aop-attach/capture-stage1/status.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/attach_status' \
  > build/periph/aop-attach/capture-stage1/status-repeat.txt
cmp build/periph/aop-attach/capture-stage1/status.txt \
  build/periph/aop-attach/capture-stage1/status-repeat.txt
```

Use a fresh capture directory per boot. Mount debugfs through the normal
workflow if needed; the snapshot already happened at probe. Do not use the
live `status`/`preflight` files or /dev/mem to work around a missing result.

Expected **shape**, values to be measured:

```text
format=1 stage=1 result=observed-mailbox-enabled-empty guard_errno=0 execution=unknown wrapper_b88=0x00020001 inbox=0x00020001 outbox=0x00020001 idle=0x00000079 hardware_writes=none fifo_reads=none irqs=none dma=none
```

2026-10-25 correction: `+b88` mirrors inbox; its old CPU-running
interpretation is withdrawn. Stage 1 never classifies the CPU from this word.
See [aop-force-cold.md](aop-force-cold.md) for the updated halt predicate.

This example uses historical values; matching them is not a new firmware
identity or ownership proof. Require identical repeated results and the same
saved line in dmesg. There must be no active-driver-armed message, AOP IIO
device from this mode, transcript TX, new IRQ handler or DMA buffer.

| Result | Interpretation / stop condition |
| --- | --- |
| `observed-mailbox-enabled-empty` | Outbox enabled and empty at snapshot time; CPU execution unknown. **End experiment**; it does not permit stage2 traffic. |
| `observed-pending-unconsumed` | Outbox not empty. **End experiment** without reading the FIFO; not evidence that the word is HELLO. |
| `aborted-domain-guard` | Wrapper skipped. Keep errno and ordinary boot logs; never force domains on or fabricate register values. |
| `aborted-mailbox-fault` | One or both fault bits latched. Keep all raw values; do not clear them. |
| `aborted-outbox-disabled` | Outbox disabled; CPU execution unknown. End; do not enable it. |
| missing file / invalid or conflicting selector | Check build/DT/cmdline/dmesg offline. No /dev/mem fallback or relaxed guard. |

Also stop for warning/oops, IRQ flood, SSH/root loss or unexplained domain
change. The guard locks software transitions but cannot prevent autonomous
gating or bound a wedged MMIO transaction. It runs after ordinary PMGR probe;
it is not an observation of the pre-Linux AON rail. Never kill the live
qemu-nbd/QEMU server by process name.

## Stage2/3 refusal checks and the later acceptance gates

An optional separate boot may replace attach_stage=1 with 2 or 3, retaining
all other modes off. Expected line:

```text
format=1 stage=2 result=refused-no-verified-host-epoch wrapper=skipped hardware_writes=none fifo_reads=none irqs=none dma=none
```

Stage3 changes only `stage=3`. These runs test refusal, not a handshake. Host
tests already establish it without device risk; they are not prerequisites
to repeat on the iPad.

The pinned receiver can re-enumerate EPMAP without clearing AFK state; this
is detailed with firmware addresses in the research report. Before implementing
real stage2, identify the actual running image and prove global old-buffer/ext lifetime
and DMA release. Then use one bounded HELLO/EPMAP-only attempt with no
STARTEP (including system endpoints), no AFK, no addresses and no power/reset
register writes. Preserve the exact transcript and abort on any deviation.
A returned EPMAP by itself must never promote to stage3.

Before real stage3, establish a fresh endpoint/AFK initialization rather
than same-buffer resume. All 0xa1 PAs must belong to bounded Linux allocations;
validate descriptors, AOP-written headers and announcement identity. Require
successful per-device calibration replies, rate replies and real 0xc2 samples
before exposing IIO; measure cadence and six poses before autorotate. Keep
every published allocation pinned through failure. Do not retry by resetting
the AOP or let another kernel reuse memory a surviving master may still use.
