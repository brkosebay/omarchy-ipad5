# AOP stage 2/3 experiment — tri49a

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25 correction: **`+0xb88` is not verified CPU status.** It mirrors
inbox `+0x4008` in every saved boot today, including FIFO occupancy. Earlier
running/halted conclusions from its bit0 are withdrawn; execution remains
unknown. The current forced-cold path accepts RUN `+0x44` bit4 clear after
10 ms, with auto-boot/outbox disabled and fault-free mailboxes, under the
owner's accepted crash risk. See [aop-force-cold.md](aop-force-cold.md).
Historical captures and earlier path descriptions below retain their provenance.


Current implementation: [stage 2d–2h and fixed chained stage 3d](aop-stage2d.md).
The stage-2b capture establishes no inbox drain; b88 bit0 alone is not an
execution proof. Stage-3b failed a resource claim despite a correct DT pool.

Next attempt: [stage 2b/2c and chained 3b/3c](aop-stage2b.md). The saved
stage-2 result is [logs/aop-stage2-20261025/](../release/OMISSIONS.md): no
crash, one speculative reply, no RX, `failed-no-epmap`. The historical
`auto_boot=0` in experiment state was not a register read; it was a placeholder.
The old entries below remain the record of that attempt.

2026-10-25. Owner accepts the crash risk and explicitly requests proceeding
without complete firmware-identity/old-DMA proof. **Experimental go under that
decision.** Host/Docker work only: no device access, commits or pushes.

## DFU analysis

The stage-1 line in `logs/aop-attach-stage1-20261025.txt` establishes a running
CPU, empty/enabled mailbox and no observed faults. It identifies neither an
RTKit image nor an AOP reset epoch. No iOS kernel runs in the current DFU-to-Linux
boot; older iOS grants are excluded only if the AOP itself was reset. The
power-off capture establishes running-at-probe, not erased AOP state. Thus the
executing firmware and whether it matches the iOS 20H350 file remain unknown.

Local pongoOS source supplies useful corrections to the older notes:
`src/boot/stage3.c:trampoline_entry` patches and executes a bootloader before
entering pongoOS; “no Apple boot stage executes” is too strong for this path.
`src/boot/patches.S` and `src/drivers/recfg/recfg_soc.c` identify A9 reconfiguration
storage in AOP SRAM, with its base read from `0x210800008`. iBoot's reconfiguration
tables are distinct from the FUD/RTKit sensor firmware. The manifest's
`IsLoadedByiBoot=false` concerns that sensor image; it does not identify the
running CPU as SecureROM, LLB, iBoot firmware or AOP ROM. No ROM attribution is
proved here. Shared Ghidra identity was rechecked as the 20H350 j71t kernelcache
SHA256 `c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.

ADT carveouts 31 (`0x210e00000/0x9b000`) and 29
(`0x210e9b000/0x5000`) are outside Linux DRAM (`0x800000000…`), so their absence
from `/reserved-memory` does **not** put them in the page allocator. tri48a's
compiled DT only has the NVMe-HMB reservation placeholder; m1n1 adds runtime
reservations. AFK grants and the separately allocated EC0p ext segment are
dynamic DRAM, absent from the ADT carveout list. The pinned receiver's type-2
branch at `0x010311f6/0x0103128a` re-enumerates endpoints without clearing AFK
state; `0x0100f5dc` retains its PA mapping. Consequently neither no iOS in this
AP boot nor HELLO/EPMAP proves that all old references are gone. Reservations
of the known SRAM cannot establish a complete DMA allowlist. The new pool
confines **new AFK grants**, not all accesses by unidentified firmware.

## What is implemented

Both stages require all other AOP modes off, including `active=0`, `debug=0`.
The active driver creates its debugfs directory automatically for stages 2/3.
Stage 1 remains the read-only observer. Stage 0 retains the older paths.

* **Stage 2:** under the five-domain read-only PMGR guard, require a running CPU,
  enabled outbox and no faults. Poll up to 5 s for HELLO. If silent, transmit
  **one HELLO_REPLY** `0x00200000000c000c` (there is no AP-originated HELLO in
  this protocol). ACK strictly increasing EPMAP chunks within endpoint 0..255,
  send AP readiness=1, retain another 2 s of replies, then freeze. No STARTEP,
  ping loop, AFK response, buffer grant, IRQ handler or firmware load.
* **Stage 3:** repeat that handshake once in its own boot, then start advertised
  application endpoints, negotiate AFK v8 and answer `0x89` with reserved pool
  PAs. Validate tags, ring extents/nonoverlap and firmware-written headers;
  start queues, discover `accel`/`0x10000007`, send available calibration then
  batch/raw/report = 100000/0/100000. Missing calibration is logged and permits
  uncalibrated experimental samples. First report admits the existing IIO
  device; debugfs also retains the latest 128 raw reports. Rate/scale/mount
  orientation still need hardware validation. System endpoints are logged,
  never started and never allocated buffers; their formats remain unresolved.
* **Memory:** fixed 4 MiB `aop-afk@858000000` (`0x858000000..0x8583fffff`),
  `no-map`, no reusable/pool compatible. Stage 3 exclusively claims it and maps
  Normal non-cacheable memory. A bounded 64-byte-aligned bump allocator serves
  every new AFK buffer; no AFK `dma_alloc_coherent` call is reachable in this
  mode. Published slices are never freed, including failed sends. An identical
  FDT **header reservation** persists through the existing warm merger even
  when rebooting into a recovery DT that predates the named no-map node.
* **Limits:** one attempt per boot, no sysfs bind interface; no cold-loader
  fallback, firmware/ext/remap/auto-boot mappings, AON power/reset writes, fault
  clears, NVRAM/syscfg/boot-storage access. Staged writes are mailbox words and,
  in stage 3, new reserved buffers/ring ownership fields. Startup traffic is
  bounded; failures halt polling and further experiment TX. A successful stage-3
  shutdown attempts rate0/AFK stop while keeping the pool reserved regardless
  of ACKs. No DMA-release claim is made.

Every experiment mailbox RX/TX is logged to dmesg. `startup_transcript` retains
the initial sequence (512 slots, bounded before streaming); `transcript` is the
latest 512 words. `samples` retains 128 raw reports with AP receipt timestamps.
`state` includes the selector, speculative flag, handshake result, endpoints,
grants, pool use, service, command replies and sample counts. Debugfs reads do
not trigger another attempt. Stage2 completion means management exchanged,
not exclusive firmware ownership.

## Build and package (host only)

```sh
python3 -B scripts/periph/aop/test-offline.py
scripts/periph/aop/build-stage23.sh
python3 -B scripts/periph/aop/package-stage23.py
python3 -B scripts/periph/aop/tests/stage23-artifacts.py
```

> Private graphics-dependent excerpt omitted from this source snapshot.

## Orchestrator commands (not run by this task)

Optional read-only warm-DT preview before booting from the stage-1 baseline:

```sh
R=build/periph/aop-stage23/preflight; mkdir -p "$R"
scripts/omarchy/ssh.sh 'sudo -n cat /sys/firmware/fdt' > "$R/running.dtb"
python3 -B scripts/boot/device/linux-a9-fdt-merge \
  --old build/kernel-tri48a/dtbs/s8003-j71t.dtb --running "$R/running.dtb" \
  --new build/kernel-tri49a/dtbs/s8003-j71t.dtb -o "$R/merged.dtb"
python3 -B scripts/periph/aop/check-stage23-memory.py --runtime --dtb "$R/merged.dtb"
```

Use the **actual running entry's pristine DTB** for `--old` if it differs from
tri48a. The normal kexec loader performs this merge too.

```sh
source build/periph/aop-stage23/entries.env
ENTRY="$AOP_S2_ENTRY" scripts/boot/kexec-host.sh "$AOP_S2_PAYLOAD"
# The collector waits for the one boot-time attempt and never starts/retries it.
scripts/periph/aop/capture-stage23.sh stage2
# Optional individual reads:
R=build/periph/aop-stage23/capture-s2; mkdir -p "$R"
scripts/omarchy/ssh.sh 'uname -a; cat /proc/cmdline; sudo -n dmesg' > "$R/boot.txt"
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop-active/state' > "$R/state.txt"
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop-active/startup_transcript' > "$R/startup.txt"
```

Stage 2: look for exactly one TX `00200000000c000c`, RX type-8 map(s), TX map
ACK(s), AP readiness, and `result=stage2-handshake-complete` with zero AFK
grants. `speculative_hello=1` means no device HELLO was observed. Silence gives
`failed-no-epmap`; retain it, do not infer the image or replay on that boot.

Stage 3 is a separate explicit boot, selectable by the one cmdline token:

```sh
ENTRY="$AOP_S3_ENTRY" scripts/boot/kexec-host.sh "$AOP_S3_PAYLOAD"
scripts/periph/aop/capture-stage23.sh stage3
# Optional individual reads:
R=build/periph/aop-stage23/capture-s3; mkdir -p "$R"
scripts/omarchy/ssh.sh 'uname -a; cat /proc/cmdline; sudo -n dmesg' > "$R/boot.txt"
for f in state startup_transcript transcript rings samples; do
  scripts/omarchy/ssh.sh "sudo -n cat /sys/kernel/debug/apple-a9-aop-active/$f" > "$R/$f.txt"
done
scripts/omarchy/ssh.sh 'sudo -n cat /sys/firmware/fdt' > "$R/running.dtb"
scripts/omarchy/ssh.sh 'sudo -n cat /proc/iomem' > "$R/iomem.txt"
python3 -B scripts/periph/aop/check-stage23-memory.py --runtime \
  --dtb "$R/running.dtb" --iomem "$R/iomem.txt"
scripts/omarchy/ssh.sh 'for d in /sys/bus/iio/devices/iio:device*; do test -f "$d/name" && { cat "$d/name"; cat "$d"/in_accel_{x,y,z}_raw 2>/dev/null; }; done'
```

Stage 3: pool claim `a9-aop-stage3-pool`, every `AFK grant` PA+size inside the
4 MiB pool, service name/id match, calibration/rate replies, `phase=streaming`,
`reports` increasing and changing axes in `samples`/IIO. Capture samples again
after tilting. `failed-no-accel-service` with no AFK init is a useful result:
management re-enumeration did not recreate AFK state. No reset/retry is added.

Keep the existing recovery/default entry. Both packaged entries carry the same
reserved DT and stay below the 1000-byte cmdline budget. A future direct DFU
boot of an older payload does not inherit this new reservation; keeping it
reserved there requires a payload with this DT. No stage has been device-tested
by this task, and successful streaming is not claimed.

## Verified build and files to commit

Final source patch SHA256:
`a009b431b88b3ab010953bbbbb07f2e6673560b8e087faf29ce38a859c31d2f7`.
Entry names are `tri49a-s2-a009b431b8` and `tri49a-s3-a009b431b8`;
both command lines are **987 bytes**. Kernel release:
`7.3.0-rc1-tri49a-s23-a009b431b8-gce3130eb4aed-dirty`.

> Private graphics-dependent excerpt omitted from this source snapshot.

Workspace files owned by this task (all left uncommitted):

```text
docs/periph/aop-stage23.md
docs/periph/ds-aop-attach.md
docs/periph/aop-attach-test-plan.md
scripts/periph/aop/build-stage23.sh
scripts/periph/aop/capture-stage23.sh
scripts/periph/aop/check-stage23-memory.py
scripts/periph/aop/package-stage23.py
scripts/periph/aop/check-build.py
scripts/periph/aop/install-kernel.py
scripts/periph/aop/test-offline.py
scripts/periph/aop/kernel/a9-aop-experiment.h
scripts/periph/aop/kernel/a9-aop.h
scripts/periph/aop/kernel/apple,j71t-aop.yaml
scripts/periph/aop/kernel/apple-a9-aop-active.c
scripts/periph/aop/kernel/apple-a9-aop.c
scripts/periph/aop/kernel/apple-a9-aop.rst
scripts/periph/aop/kernel/s8003-j71t-aop.dtsi
scripts/periph/aop/tests/experiment.c
scripts/periph/aop/tests/stage23-artifacts.py
scripts/periph/aop/tests/state-probe.c
```

Kernel files are the nine entries in `build/kernel-tri49a/DIRTY` (eight
modified plus `drivers/soc/apple/a9-aop-experiment.h`). The complete overlay
is `build/kernel-tri49a/SOURCE.patch`, mirrored as
`build/periph/aop-stage23/aop.patch`; apply it to tri48a when making the kernel
commit. Canonical source is also installed on a9/aop. No other worktree or
pre-existing workspace edits belong to this handoff.
