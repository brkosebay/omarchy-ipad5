# AOP inherited ring reader — orchestrator handoff

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Implementation for E1/E2 in [ds-orientation-paths.md](ds-orientation-paths.md).
No iPad commands were run by the implementation agent. No workspace commit or
push is part of this handoff. Kernel changes belong only to `a9/aop` in the
`linux-a9-src` Docker volume. Patches are relative to Round 7 commit
`b0fbdf663609d05c1523a4d61f631eea43867471`.

## Initial offline implementation result (2026-10-24, before device follow-up)

Committed only on kernel `a9/aop`: `b8a4acf3a1` (binding), then
`286ae86efe` (reader). At that handoff the kernel worktree was clean and the
exported patches replayed to that HEAD and passed checkpatch with
zero errors and zero warnings each. No workspace commit or push was made.

Full LLVM=1 arm64 W=1 Image.gz/DTBs and final source compilation passed.
Existing warnings outside the AOP changes remain in `build-w1-full.log`.
IIO-disabled and IIO-modular E1 compilation passed without IIO dependencies.
The ASan/UBSan harness, explicit DT-hint/kexec-merge tests, binding schema and
linked-symbol/gate checks passed. Final build artifacts are in
`build/kernel-aop/`; logs are in `build/periph/aop-ring-reader/`.
No device streaming, cache coherency or six-pose measurement was performed.

## Device follow-up: continue beyond unreadable RAM

Verified follow-up commit: `d638469c63201f26a9c2579f861680a84543a28c`
on kernel `a9/aop`. LLVM=1 arm64 W=1 Image.gz/DTBs, the production-loop
fault-injection harness, IIO-disabled/module visibility builds, linked-symbol
and gate checks passed. The linked initcall is `rr_early_init2s` (postcore sync).
All three exported patches pass checkpatch with zero errors and warnings and
replay exactly to HEAD. The kernel tree and exported build provenance are clean.
No device access, push or workspace commit was performed by this agent.


User-reported measurement: combined `nextint-1d6958e823` included
`a9/aop 286ae86efe`; its initial Linux cold boot followed an iOS session, but
E1 ran after a warm kexec. The old scanner stopped at
`fault_pa=0x00000008035f0000`, `errno=-14`, `bytes=56557568`, `hits=0`,
`elapsed_ms=1092`. This is incomplete coverage; it does not establish that no
inherited ring exists. The first Linux kernel already reused some RAM.

The follow-up skips protected and faulting pages, continues, and reports
format 2 coverage. Fault recovery does not change mappings, reserve pages,
clear exceptions in hardware, or use cache maintenance. Page quarantine also
prevents another candidate or watch from retrying the same failed PFN.
Skipped and readable sets are disjoint even if a watch later faults on a
previously readable page. A bad hit is dropped without stopping other watches.

Read `bytes` (readable), `skipped`, `ram_bytes`, `walked_bytes`,
`unvisited_bytes`, `fault_pages`, per-reason totals, and the `skip` lines.
Range ends are exclusive. At most 256 ranges are retained; any omitted range
bytes are explicitly reported in `omitted_skipped_bytes`, while totals and
quarantine stay exact. `walk_complete=1` means every admitted full RAM page was
classified, including skips, not that every byte was readable. A budget,
hit-limit or signal exit retains the coverage obtained so far. Tracking admits
at most a 4 GiB DRAM address span and the page walk is capped at 2 GiB.

For best evidence, put these options in the FIRST Linux cold-boot payload
immediately after iOS -> DFU -> Linux (no intermediate Linux or AON cycle):

```text
apple_a9_aop.debug=1 apple_a9_aop.ring_scan=1 apple_a9_aop.ring_scan_early=1 apple_a9_aop.active=0 apple_a9_aop.attach=0
```

Then, on the device, collect the frozen report:

```sh
sudo cat /sys/kernel/debug/apple-a9-aop/ring_scan
```

Or from the orchestrator Mac:

```sh
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/ring_scan'
```

A second read must match byte for byte. Expect `phase=postcore-initcall` and
`scan_stop=end-of-ram` if time/byte/hit limits were not reached. Read faults no
longer make global `errno=-14`; they appear as `reason=read-fault` and in
`fault_pages`. Do not infer the cause of the old fault from its address alone.
Without the early flag, the same command performs the lazy scan and reports
`phase=debugfs`. Early mode still starts after allocator initialization and
cannot recover bytes overwritten by bootloader/kernel allocations or a prior
Linux session. If early capture cannot allocate, it reports
`phase=early-unavailable` instead of silently performing a late scan.

Regression coverage compiles the actual kernel E1 page loop, read callback,
copy splitter and skip-accounting functions with an ASan/UBSan fake RAM
backend. It injects the exact reported fault at `0x8035f0000`, verifies a ring
is found later, checks no retry of bad/reserved/no-map pages, cross-page faults,
watch-time coverage revocation, coalescing/range truncation, pacing and
byte/time/hit/signal limits. No device commands are executed by these tests.

## Reserved-DRAM discovery follow-up

Verified commit: `9f0ddbb8943fc12cae5e052f0f4b9188de6e4af9` on `a9/aop`.
Full LLVM=1 arm64 W=1 Image.gz/DTBs, production policy/classifier/loop
ASan/UBSan tests, IIO-disabled/module and CMA-disabled builds, linked
postcore-initcall and immutable parameter/gate checks passed. All four
exported patches replay to HEAD and pass checkpatch with zero errors and
warnings. Build provenance is clean. No workspace commit, push or device
access was performed.


The saved [nextint3 early capture](../release/OMISSIONS.md)
for `d638469c63` completed its page walk with zero faults and zero hits:
1,548,091,392 readable bytes, 525,762,560 bytes skipped as reserved, and
2,097,152 no-map bytes. That boot came from Linux -> DFU without a new iOS
session. The [ring-location study](ds-aop-ring-location.md) establishes the
ring as a contiguous iOS kernel-heap DRAM buffer allocated in response to
AFK 0x89. It does not live in the AOP firmware window. A no-hit scan without
the required iOS session does not settle whether a live inherited ring exists.

New default-off `apple_a9_aop.ring_scan_reserved=1` admits mapped reserved
System RAM in E1, including crash-kernel, CMA and other non-no-map
reservations. It only relaxes the memblock/PageReserved exclusion; positive
RAM, resource and linear-map checks, self/stack exclusion, page-confined
fault handling, quarantine, pacing, hit/byte/time bounds all remain.
Every hit includes PA and `range_class=system-ram|crash-kernel|cma|reserved-dram|mixed`.
Classes come from kernel resource descriptors, CMA-area metadata and
memblock/PageReserved state, not addresses inferred from the boot log.
A hit spanning different page classes is mixed. The classes describe
membership and do not prove untouched contents or AOP ownership.

Existing readable/skipped/unvisited totals and skipped-reason/range lines
are retained, with an additional frozen `ring_scan_reserved=0|1` line.
On an eligible expanded scan, reserved pages move from the skipped count
into readable bytes; no-map and faulting pages still remain skipped. There
is no fixed expected coverage percentage if other exclusions or limits occur.
E2 is unchanged and still rejects crash-kernel/CMA as its own reservation.

For the next FIRST Linux boot after an actual iOS session with accel enabled,
enter DFU without cycling AON, then keep the existing protected boot cmdline
(including `crashkernel=256M`) and set these exact AOP tokens:

```text
apple_a9_aop.debug=1 apple_a9_aop.ring_scan=1 apple_a9_aop.ring_scan_early=1 apple_a9_aop.ring_scan_reserved=1 apple_a9_aop.active=0 apple_a9_aop.attach=0 apple_a9_aop.ring_iio=0
```

After boot, read (do not trigger another boot first):

```sh
sudo cat /sys/kernel/debug/apple-a9-aop/ring_scan
```

Require `phase=postcore-initcall`, `ring_scan_reserved=1` and a complete
`snapshot_end=1` report. Preserve all hit PAs/classes, header bytes, parse
results and observed wptr changes; only an advancing 0xc2 report is evidence
of the accel stream. Repeated reads must match the frozen capture. The
host-only preparation helper supports `--mode scan --early --reserved` and
forces active/attach/IIO off without changing non-AOP baseline arguments.
No device commands were run by this implementation agent.

The ASan/UBSan harness now compiles the production page policy and classifier
as well as the copy/walk functions. Tests cover default exclusion and opt-in
admission of memblock and PageReserved pages, all five classes, discontiguous
CMA membership, a mixed cross-page ring, no-map crash-like ranges, invalid
PFNs/maps, non-RAM resources, self pages, direct AOP-window rejection and
reserved-page fault quarantine. The real early postcore initcall remains
linked; source and bootargs checks cover the new immutable parameter.

## What is implemented

- E1: `apple_a9_aop.debug=1 apple_a9_aop.ring_scan=1` exposes a root-only,
  one-shot `ring_scan`. By default the first read scans eligible System RAM;
  `apple_a9_aop.ring_scan_early=1` instead captures at postcore initcall before
  platform device population. Later reads only format the frozen result.
  Limits: 2 GiB of traversal, 45 seconds,
  16 hits, about 64 MiB/s plus processing, then a shared two-second watch.
  Read faults quarantine the failing page and continue; all E1 copies are
  page-confined. Memblock-reserved, PageReserved pages are skipped by default; `ring_scan_reserved=1` admits
  them after the normal RAM/map checks. No-map pages are always skipped.
  E2 still latches faults.
  The reader's own allocations and current kernel stack are excluded to avoid
  discovering its copied headers. Discovery and watch baselines are separate;
  `advanced=yes` only counts changes within the observation window.
- E1 reports the PA, exact header signature, rptr/wptr, observed wptr changes,
  first 256 bytes, bounded report decode, and lockless current page state.
  Nonreserved allocator pages, including currently free pages, trigger a
  critical candidate warning; advancing candidates trigger a LIVE warning.
  `referenced_pages` reports nonzero current references. `PageBuddy` only
  identifies free-block heads; a zero reference count does not establish
  that a page has never been used. PageReserved also does not prove AOP ownership.
- E2: `ring_iio=1` only accepts the explicitly supplied early DT `no-map`
  reservation, maps it normal WB with `PAGE_KERNEL_RO`, revalidates the exact
  signature and requires a new valid motion report within two seconds.
  Three raw axes, scale `0.000149638`, and the residual mount matrix are
  exposed under `apple-a9-aop-ring-accel`. No guessed sampling rate is exposed.
  Direct reads return `ENODATA` after 500 ms without fresh samples/wptr.
  Malformed data or a copy fault latches failure; no retry.
- Directed capture: `ring_pa`/`ring_pa_size` read up to four exact RAM ranges
  (64 KiB each) twice at postcore, one second apart, and expose frozen bytes,
  offset signatures, range metadata and byte differences in `ring_pa`.
- Any ring mode prevents active-driver probe, even with `active=1`.
  In ring mode the observer skips wrapper mappings and PMGR lookups entirely.
  There are no mailbox/FIFO accesses, IRQ requests, hardware/rptr writes,
  cache maintenance, DMA allocation or late reservation. All modes default off.

This is a measurement path, not a claim that auto-rotate works on the iPad.
A read-only consumer cannot acknowledge shared rptr: a producer that requires
space reclamation will eventually stop on a full ring. Static/no-hit results
also cannot prove the AOP has stopped DMA elsewhere. Other AOP buffers remain
uncontained; reserving one ring does not fix all inherited DMA.

The scale is the documented signed 16.16 g interpretation of calibrated AOP
MPU-6900 reports, not 16-bit chip register counts. Confirm it with six poses.
The default mount matrix is identity and must be measured in screen coordinates;
firmware already applies ARot, so do not apply it again.

## Offline reproduction

Run from the workspace on the Mac; these commands have no device access:

```sh
bash scripts/periph/aop/build-ring-reader.sh
bash scripts/periph/aop/check-ring-variants.sh
python3 -B scripts/periph/aop/test-offline.py
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aop'
scripts/build-kernel.sh --run aop 'git log -2 --oneline; git status --short'
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/export-ring-reader.py'
(cd patches/aop/ring-reader && shasum -a 256 -c SHA256SUMS)
```

Full W=1 build output is retained at
`build/periph/aop-ring-reader/build-w1.log`. The W=1 wrapper exports the shared
builder's artifact/provenance format:
`build/kernel-aop/{Image.gz,dtbs,config,COMMIT,DIRTY,SOURCE.patch,RELEASE}`.
The standalone aop kernel is not a complete Omarchy/NVMe/kexec integration.
Checkpatch runs on each exported patch, keeping DT bindings separate:

```sh
scripts/build-kernel.sh --run aop 'for p in /work/patches/aop/ring-reader/*.patch; do scripts/checkpatch.pl --no-tree "$p" || exit; done'
```

The ASan/UBSan harness compiles the actual const snapshot decoder. It covers
header alignment and exact signature, signed axes, both known entry magics,
report-category rejection, tail-header wrap, exact-end wrap, publication
bounds, overflow/truncation, atomic rejection of malformed trailing subpackets,
20,000 mutation trials and byte-for-byte unchanged input, including rptr/wptr.
Host tests also cover explicit DT hints, covering pages for a non-page-aligned
ring, malformed PA refusal, existing-hint refusal and protected bootargs.
The actual kexec FDT merger is exercised with runtime RAM fixups, colliding
phandles and preservation of the same reservation on a second warm transition.
These checks do not measure DMA coherency or live producer behavior.

## Integration: host only

> Private graphics-dependent excerpt omitted from this source snapshot.

```sh
INTEGRATION_TREE=aopringint scripts/integrate.sh --check \
  a9/periph a9/pcie 97313d95c3 a9/touch a9/audio a9/nvme-install \
  a9/power a9/idle a9/boot-pin a9/aop
INTEGRATION_TREE=aopringint scripts/integrate.sh \
  a9/periph a9/pcie 97313d95c3 a9/touch a9/audio a9/nvme-install \
  a9/power a9/idle a9/boot-pin a9/aop
scripts/build-kernel.sh --run aopringint 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aopringint'
python3 -B scripts/periph/aop/prepare-ring.py --tree aopringint --mode scan --early \
  --base-cmdline build/boot/integ-entry/cmdline \
  --out build/periph/aop-ring-e1
```

The helper preserves all non-AOP baseline arguments, requires
`apple_pmgr.kexec_keep_on=1 panic=0 apple_wdt.cold_restart=0`, replaces the entry
name, and forces active/attach off. It refuses dirty/standalone builds and
requires the NVMe/kexec and 16 KiB configuration. It never stages or boots.
If this integration conflicts, stop and resolve it on the owning workstream;
no automatic conflict resolver or integration commit is supplied here.

Package the E1 cold-boot payload, retaining the baseline initramfs and EL3/PSCI
m1n1. In the following zsh blocks `${=bootargs}` intentionally splits the helper's
unquoted-whitespace-free command line into arguments:

```sh
bootargs=$(cat build/periph/aop-ring-e1/cmdline)
TREE=aopringint M1N1="$PWD/build/m1n1-boot/m1n1.bin" M1N1_VARS=psci=1 \
  PAYLOAD_OUT="$PWD/build/periph/aop-ring-e1/m1n1.bin" \
  scripts/mkpayload.sh build/boot/initramfs-omarchy.gz ${=bootargs}
```

## E1: first device run must follow iOS

AOP state belongs to the **last iOS boot**. An arbitrary already-running Linux
session, a real AON power cycle, or an earlier active-driver test is not a
substitute. Boot iOS normally, unlock it and leave accel reporting enabled
(e.g. verify that orientation changes). Then enter DFU with buttons without
power-cycling AON and run, as orchestrator:

```sh
scripts/boot.sh pongo
scripts/boot.sh linux build/periph/aop-ring-e1/m1n1.bin
mkdir -p build/periph/aop-ring-e1/capture
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline' \
  > build/periph/aop-ring-e1/capture/identity.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/ring_scan' \
  > build/periph/aop-ring-e1/capture/scan.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/ring_scan' \
  > build/periph/aop-ring-e1/capture/scan-repeat.txt
cmp build/periph/aop-ring-e1/capture/scan.txt \
  build/periph/aop-ring-e1/capture/scan-repeat.txt
scripts/omarchy/ssh.sh 'sudo -n dmesg; cat /proc/iomem; ls /sys/kernel/debug/apple-a9-aop' \
  > build/periph/aop-ring-e1/capture/kernel.txt
```

If debugfs is not mounted, mount it before the read with
`scripts/omarchy/ssh.sh 'sudo -n mount -t debugfs debugfs /sys/kernel/debug'`.
The early scan may delay boot by roughly 45 + 2 seconds. Reading debugfs then
only formats the saved capture: require `phase=postcore-initcall`. Without the
early flag, allow at least a minute in the SSH collector and start promptly.
Do not run stress/allocation tests on this unreserved boot.

Require `snapshot_end=1`, `errno=0`, the exact signature, `advanced=yes`,
`parse_errno=0`, `reports>0`, subtype 0xc2 and plausible gravity-like axes.
Both AOP/IOP magic values are accepted; magic alone does not prove RX direction.
If the search stops at a budget/hit limit, record that it was incomplete.
No wptr change over two seconds is an observation, not proof that the ring
never streamed: it might already be full. A no-hit result might mean Linux
reused it or coherency is absent. Do not enable any mailbox/attach fallback.

Critical `LIVE ... Linux-owned RAM` output is evidence of the latent corruption
risk. Preserve PA, page-state counts and boot ID. A current free page is still
unsafe for ongoing firmware DMA. Do not attempt late reserve/pin/repair or
claim the scan establishes historical ownership. The broad scan exposes only
matched rings; the separate opt-in directed capture below accepts bounded
boot-time PA lists and freezes its exact-range output at postcore.

## Directed PA capture: exact AFK buffer from the last iOS session

Kernel commit: `c225239c9069b600b2d5a3339dc6b29ff10b9bd3` on `a9/aop`,
exported as patch 5. Full LLVM=1 arm64 W=1 Image.gz/DTBs and the production
capture ASan/UBSan tests pass. Build provenance is clean. The implementation
agent performed no device access, push or workspace commit.
IIO-disabled/module and CMA-disabled translation-unit checks, linked postcore
and parameter checks, replay of all five patches and SHA256 verification pass.
Every exported patch has zero checkpatch errors and warnings.

> Private graphics-dependent excerpt omitted from this source snapshot.

After integrating the latest `a9/aop` as described above, prepare an exact
capture without adding a DT reservation. In zsh, enter the actual hex values
from the iOS `PA buf[i]` line:

```sh
read 'afk_pa?Buffer PA from the current iOS aop-ring probe (0x...): '
read 'afk_size?Buffer size from that same probe (0x...): '
python3 -B scripts/periph/aop/prepare-ring.py --tree aopringint --mode target \
  --buffer "${afk_pa}:${afk_size}" \
  --base-cmdline build/boot/integ-entry/cmdline \
  --out build/periph/aop-ring-target
cat build/periph/aop-ring-target/cmdline
bootargs=$(cat build/periph/aop-ring-target/cmdline)
TREE=aopringint M1N1="$PWD/build/m1n1-boot/m1n1.bin" M1N1_VARS=psci=1 \
  PAYLOAD_OUT="$PWD/build/periph/aop-ring-target/m1n1.bin" \
  scripts/mkpayload.sh build/boot/initramfs-omarchy.gz ${=bootargs}
```

Repeat `--buffer PA:SIZE` for up to four buffers (1..65536 bytes each). The
helper preserves non-AOP baseline arguments, forces active/attach/IIO/broad
scan off, and leaves the pristine DTB unchanged. For a manually prepared
payload, replace existing AOP arguments with these tokens, substituting actual
values for the two angle-bracket placeholders:

```text
apple_a9_aop.debug=1 apple_a9_aop.ring_pa=<PA_FROM_IOS> apple_a9_aop.ring_pa_size=<SIZE_FROM_IOS> apple_a9_aop.ring_scan=0 apple_a9_aop.active=0 apple_a9_aop.attach=0 apple_a9_aop.ring_iio=0
```

Matching comma lists are also accepted: one `ring_pa` list and one
`ring_pa_size` list with equal lengths. Repeating a parameter replaces its
list. No `ring_scan_early` flag is needed: the directed snapshots always run
at postcore, before any optional broad scan or reader allocations, with one
shared 1000 ms wait between the first and second snapshots. Bootloader and
initial kernel allocations still precede capture. The source is never
reserved, pinned or written; the bounded capture uses about 0.5 MiB of BSS.

Boot from DFU without cycling AON, then collect twice:

```sh
scripts/boot.sh pongo
scripts/boot.sh linux build/periph/aop-ring-target/m1n1.bin
mkdir -p build/periph/aop-ring-target/capture
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline; sudo -n dmesg; cat /proc/iomem' \
  > build/periph/aop-ring-target/capture/identity.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/ring_pa' \
  > build/periph/aop-ring-target/capture/pa.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/ring_pa' \
  > build/periph/aop-ring-target/capture/pa-repeat.txt
cmp build/periph/aop-ring-target/capture/pa.txt \
  build/periph/aop-ring-target/capture/pa-repeat.txt
```

Require `phase=postcore-initcall captured=1` (on separate output lines),
`config_errno=0`, expected PA/size and `snapshot_end=1`. Read each sample's
errno, System RAM/reserved/no-map membership and covering/linear page counts.
No-map, MMIO, non-System RAM, invalid mappings and reader/stack pages refuse
the whole range before source reads. Reserved mapped System RAM is admitted
here without `ring_scan_reserved=1`. Page-confined nofault copies quarantine
faulting pages across all buffers and both samples. Failed partial snapshots
are hidden and comparison becomes unavailable; other ranges continue.
Successfully captured unreserved RAM produces a critical kernel warning
about possible inherited DMA into Linux allocator pages. Current reservation
metadata cannot establish historical allocation; nothing reserves or repairs
the buffer on this boot.

Each successful snapshot dumps the first min(size,0x100) bytes and every
non-zero 64-byte line (including a short final line). It checks the existing
header signature at zero and every relative 0x40-aligned offset with enough
header bytes. A match reports offset, absolute PA, fields and whether the
full 16 KiB ring fits. `changed=yes/no/unavailable`, changed-byte/line counts
and `delta_ms` compare all requested bytes. A change does not identify its
writer; static data does not prove AOP stopped. No cache maintenance occurs.
Repeated debugfs reads only format frozen memory. Capture allocates no
buffers; its BSS pages are excluded from both directed and broad scans.

Offline verification compiles the production getters, guards, splitter,
capture and signature functions against a fake RAM backend with ASan/UBSan.
It checks unaligned exact reads, partial tails, signatures inside a buffer,
one-second mutation, multiple lists, byte bounds, all RAM refusal classes,
fault quarantine and frozen repeat capture. The packaging test verifies
matching PA/size lists, bounds, baseline preservation and active/scan gates.

## E2: explicit reservation, next DFU-to-Linux boot

Select the candidate PA from the E1 evidence and pass that exact value; the
helper never chooses a hit. The following is an interactive orchestrator prompt,
not an example fixed PA:

```sh
read 'ring_pa?PA of advancing accel RX ring from E1 (0x...): '
python3 -B scripts/periph/aop/prepare-ring.py --tree aopringint --mode iio \
  --ring-pa "$ring_pa" --base-cmdline build/boot/integ-entry/cmdline \
  --out build/periph/aop-ring-e2
cat build/periph/aop-ring-e2/reservation.dtsi
cp build/periph/aop-ring-e2/dtb \
  build/kernel-aopringint/dtbs/s8003-j71t-aop-ring.dtb
bootargs=$(cat build/periph/aop-ring-e2/cmdline)
TREE=aopringint J71T_DTB=s8003-j71t-aop-ring.dtb \
  M1N1="$PWD/build/m1n1-boot/m1n1.bin" M1N1_VARS=psci=1 \
  PAYLOAD_OUT="$PWD/build/periph/aop-ring-e2/m1n1.bin" \
  scripts/mkpayload.sh build/boot/initramfs-omarchy.gz ${=bootargs}
```

Review the hint's full page coverage and avoid bootloader/kernel/initrd/crash
placements over this range. Generic OF processing excludes it before the new
kernel allocates. It must be plain no-map RAM, never MMIO, reusable/CMA/pool,
or a region owned by another driver. The driver also rejects overlapping
reserved-memory siblings and other memory-region references. The helper
leaves the normal pristine DTB unchanged and writes a separate reviewed DTB.

**Do not kexec E1 -> E2 as an ownership fix.** The old allocator and kexec loader
can still use the unreserved ring before the incoming DT takes effect. Enter
DFU directly from E1 using buttons and boot E2 while retaining the same AOP
session, without another iOS boot or AON cycle. If iOS runs again, the PA is
stale: discard the hint and redo E1.

```sh
scripts/boot.sh pongo
scripts/boot.sh linux build/periph/aop-ring-e2/m1n1.bin
mkdir -p build/periph/aop-ring-e2/capture
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; cat /proc/cmdline; sudo -n dmesg; cat /proc/iomem' \
  > build/periph/aop-ring-e2/capture/boot.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/ring_reader' \
  > build/periph/aop-ring-e2/capture/reader.txt
scripts/omarchy/ssh.sh 'for d in /sys/bus/iio/devices/iio:device*; do
  [ "$(cat "$d/name")" = apple-a9-aop-ring-accel ] || continue
  echo "$d"; cat "$d"/in_accel_{x,y,z}_raw "$d"/in_accel_scale "$d"/in_accel_mount_matrix
 done'
```

Require the expected PA, bufsize/marker, fresh=1, new samples and early
reserved-memory log. Read the three axes repeatedly while rotating through six
poses. One axis should be near ±65536 at rest and the others near zero;
measure residual screen orientation. There is no temperature or rate ABI.
A static source refuses registration; a source that later fills/stops must
produce `ENODATA` within about 500 ms plus the polling interval. `ring_reader`
remains available on refusal for its errno. A bad hint/signature, unreserved or
ordinary mapped RAM must never lead to IIO or a late reservation.

## Warm kexec: only after E2 is already reserved

Keep the same AOP session and the same reviewed reservation in both kernels.
Stage the matching initial cold entry first so the existing FDT merge knows its
pristine DTB. This preserves m1n1's actual RAM/PSCI/calibration fixups. Do not
pass a pristine DTB directly to the raw kexec syscall or reset genpd retention.

```sh
entry=$(cat build/periph/aop-ring-e2/entry)
TREE=aopringint DTB=s8003-j71t-aop-ring.dtb \
  scripts/boot/stage-kernel.sh "$entry" \
  --cmdline "$(cat build/periph/aop-ring-e2/cmdline)"
ENTRY="${entry}-warm" scripts/boot/kexec-host.sh \
  build/periph/aop-ring-e2/m1n1.bin
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; cat /proc/cmdline;
  sudo -n cat /sys/kernel/debug/apple-a9-aop/ring_reader; sudo -n dmesg' \
  > build/periph/aop-ring-e2/capture/warm.txt
```

Require a new boot ID, unchanged ring PA/reservation, matching signature, zero
AOP traffic and fresh samples again before declaring warm success. If already
full/static, refusal is expected and no reader can revive it without changing
the protocol. A subsequent iOS boot invalidates this PA even if it coincidentally
looks plausible. Never infer that DFU or kexec recreated an iOS session.

After six-pose scale/matrix validation and sustained streaming evidence, run
`ipad-autorotate --probe` on the device before enabling rotation policy. Keep
manual rotation available. No device outcome is asserted by this handoff.
