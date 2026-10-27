# ANS warm-kexec link failure: powered-reset trial

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Research sub-agent, 2026-10-25. Read `AGENTS.md` and the
[previous report](kexec-hang-2026-10-25.md) first. No device access, workspace
commits, pushes, payload installation, or default promotion.

Kernel commit **`a412bc8f04aeb9ac04796064d127db59811510fc`**, only on
**`a9/kexecfix`**, directly after routing guard `6d449cccb3`, based on tri46
`4dd43c040f`. Export:
[`0020-PCI-apple-h9p-add-an-opt-in-powered-ANS-kexec-reset.patch`](../../patches/boot/kernel/0020-PCI-apple-h9p-add-an-opt-in-powered-ANS-kexec-reset.patch).
**The physical trigger remains unresolved. This implements an opt-in remedy
for a controlled hardware trial; it does not establish a hardware fix.**

## What the complete capture establishes

The incoming kernel cannot train **ANS port 0**, before PCI scanning, ANS
preinit, controller enable, queues, MSI delivery or root mounting. The failure
settles in actual LTSSM **3 / `POLL_COMPLIANCE`**, with LINKSTS=4 (UP clear),
PERST released, port power/ready asserted and PHY lane-ready mask 7.
This is a physical link restart failure, rather than an NVMe RDY timeout.
`POLL_COMPLIANCE` is an observed state, not proof of why the endpoint/PHY
entered it; an explicit compliance request and an unresponsive training
partner cannot be distinguished from these registers alone.

Reproduce the comparison without a device:

```sh
python3 -B -I scripts/boot/analyze-kexec-ans.py
```

The script selects the collector's **first `--- dmesg` section**, excludes
historical timestamps behind logger wrappers and nested replay, and emits
line-numbered extracts plus `summary.json` in
`build/boot/kexecfix-research/analysis/`. It retains primary rescue messages.

Evidence in [the complete capture](../release/OMISSIONS.md):

- **Line 7258, 12.352814 s:** incoming port 0 has
  `RC124=0 RC114=0x100 RC100=0x1000010`; all three bit-0 controls are off.
- **Lines 7388–7534, 12.773319–13.29 s:** first port-0 training poll fails
  with LINKSTS=4 and actual `POLL_COMPLIANCE`. The second attempt also fails.
- **Lines 7824–13408, 14.36–35.95 s:** one 100 ms off-hold boot retry,
  current-speed retrain, Gen1 retrain, three one-second off-hold Gen1 cycles,
  then shared-PHY tunable replay plus another Gen1 cycle all fail. There are
  **nine link-wait attempts: seven full bring-ups and two retrains**. No
  current-boot port-0 UP/L0 or NVMe enumeration occurs. Repeating those same
  operations or merely adding a larger root wait has no supporting success
  observation here.
- **Lines 10752/10754/10785:** a **single-wrapper previous-console record**
  reports outgoing SHST complete, `CC=0 CSTS=0 BME=0 mode=1`, then
  `handoff-down RC124=0 RC114=0x100 RC100=0x1000010 PERST=asserted`.
  These are prior-boot shutdown evidence, not incoming successes. The values
  agree with the current incoming certificate. The outgoing source already
  drains I/O/FIFO, disables the controller, blocks DMA, retires DART/SART,
  asserts PERST and stops LTSSM before removing port power/refclk.
- **Lines 13452–13479, 36.36–36.67 s:** the quoted
  `inherited ... DETECT_QUIET`, subsequent `link up`, and generic
  `removing 2.5GT/s ... / retraining failed` are **port 1 / Wi-Fi**, root
  bridge `0000:00:01.0`. They occur after ANS has exhausted recovery.
- **Line 17592, 134.002009 s:** no usable NVMe root; rescue remains alive.

> Private graphics-dependent excerpt omitted from this source snapshot.

The generic warning comes from `pcie_failed_link_retrain()` during PCI
scanning. Its attempted speed restoration is described in the
[upstream PCI quirk source](https://linux.googlesource.com/linux/kernel/git/torvalds/linux/+/bc62f5b308cbdedf29132fe96e9d591e526527e1/drivers/pci/quirks.c).
Disabling this quirk cannot repair this capture's earlier missing ANS link.
The existing Gen1 ANS fixup still prevents a recovered ANS port's cached
speed ceiling from being lifted.

`ANS snapshot mode=host` is selected by the diagnostic shell helper's
`mode=host` default in `scripts/boot/device/linux-a9-ans-state`; it means
host-only diagnostic access. The capture's sysfs value of
`ans_boot_mode` is actually `handoff`. There is no demonstrated mode mismatch.

## Source-level cause and chosen change

The concrete lifecycle problem is that **`handoff` currently performs a full
port power/refclk teardown, followed by another full incoming off/on edge**.
It is no longer a powered handoff. The outgoing controller-disable contract
is already present and observed; reintroducing live CC.EN/old DMA state would
undo earlier fixes. The remaining hypothesis is an ANS endpoint/PHY/clock
restart problem caused or exposed by those edges on retained hardware.
Readiness bits cannot prove analog clock quality or the S3E firmware state.
No evidence identifies a safe AP-side NAND/S3E rail reset; the earlier
[PCIe study](../release/OMISSIONS.md) records that limitation.

The lowest-risk next trial is to remove those power/refclk edges while
retaining endpoint reset and all DMA/controller safety checks. New boot
argument, **default off**:

```text
pcie_apple_h9p.ans_boot_mode=handoff pcie_apple_h9p.ans_keep_power=1
```

Only `drivers/pci/controller/pcie-apple-h9p.c` changes in the kernel:

1. Outgoing ANS still completes the existing I/O/FIFO/SHST/CC.EN=0/RDY=0/
   BME=0/DART-inactive sequence. H9P still blocks routing, asserts PERST,
   verifies LTSSM stopped within 250 ms, and disables SART while powered.
2. With the option in handoff mode, retain port power/refclk and force the
   existing NOGATE bit. Check powered, ready, reset-control, NOGATE and PERST
   readbacks before allowing the jump. Log `handoff-powered ...
   PERST=asserted; incoming=powered-reset`. A failed certificate panics as the
   existing unsafe-shutdown checks do.
3. Incoming takes the powered path only with the option, handoff mode, the
   `linux,booted-from-kexec` marker, all powered/ready/NOGATE readbacks, and
   **PERST already asserted**. Cold boots, old outgoing kernels or incomplete
   state use the original full bring-up.
4. The powered path still asserts/holds PERST for 100 ms, stops LTSSM,
   cancels PME_TO, programs link controls/tunables/MSI/PHY, releases reset,
   and requires fresh UP/L0 before enumeration. It skips only power/refclk
   removal/reapplication and NVMe-mode reprogramming. No old queue, HMB/TCB
   ownership, or translation table is adopted; incoming ANS preinit rebuilds
   its normal DART/NVMMU and controller/queue state.
5. `-ETIMEDOUT`/`-ENOLINK` falls back to the actual full-cycle helper, inside
   the existing two-attempt outer quota. Persistent powered/full failure can
   therefore make four initial attempts, before the unchanged bounded boot
   retry/deep ladder. Local errors return directly. Explicit deep recovery
   always uses full cycling. Wi-Fi and shared PMGR/DART domains are untouched.

`ans_keep_power=0` preserves the current policy. `ans_boot_mode=full-cycle`
also preserves full cycling even if the new option is present. The option
is read-only after boot. No new mode, physical address, reset register, rail
toggle, or PCI core change is introduced. The existing late-port routing
guard `6d449cccb3` remains necessary on genuine ANS failure.

## Offline validation

Passed on `a9/kexecfix`:

- ARM64 `drivers/pci/controller/pcie-apple-h9p.o` compilation with the existing
  tri46 config, debug info off. **No full Image/payload build or hardware
  boot was performed.**
- All **23** existing production-C ANS handoff tests, including full-cycle
  polling failures, shutdown ordering, DART/NVMMU, queues and recovery.
- Both existing missing-ANS/DART-routing tests.
- Two new production-C ASan/UBSan tests covering no power/refclk edge on
  powered reset, 100 ms PERST, per-port setup, the real full-cycle fallback,
  finite exhaustion, rejected cold/partial states, shutdown certificates,
  default/full-cycle/Round7 controls and Wi-Fi isolation.
- Both new tests fail against parent `6d449cccb3` at the intended outgoing
  and incoming power-retention assertions. This is a regression check,
  not a simulated proof of the physical cause.
- Exported patch: strict checkpatch **0 errors/warnings/checks**.
- Offline parser fixture: rejects historical/nested replay and later
  collector sections while retaining the primary rescue message.

Reproduce:

```sh
scripts/build-kernel.sh --run kexecfix \
  'python3 -B -I /work/scripts/boot/test-ans-keep-power-offline.py --source /src/wt/kexecfix'
scripts/boot/test-ans-handoff-offline.sh --source /src/wt/kexecfix
scripts/build-kernel.sh --run kexecfix \
  'python3 -B -I /work/scripts/boot/test-kexecfix-offline.py --source /src/wt/kexecfix'
scripts/build-kernel.sh --run kexecfix \
  'make -j2 LLVM=1 ARCH=arm64 drivers/pci/controller/pcie-apple-h9p.o'
```

Logs: `build/boot/kexecfix-research/{compile,final-tests,baseline-regression}.log`.

## Orchestrator hardware test

> Private graphics-dependent excerpt omitted from this source snapshot.

## Would enabling the boot watchdog automatically boot known-good?

**No, not by itself.** `CONFIG_APPLE_J71T_BOOT_WATCHDOG=y` plus
`apple_boot_watchdog.timeout=120` arms a software timer at `subsys_initcall`.
It calls `panic()` on expiry; it neither programs a hardware reset nor
selects a boot entry. Normal initramfs root success disarms it in
`linux_a9_early_storage_stop()`. The failed capture has
`kexec_loaded=0`, **`kexec_crash_loaded=0`**, a 256 MiB crash reservation and
`panic=0` (lines 330–340). Reservation is not an armed crash image. With the
watchdog enabled, this responsive rescue would become a panic and then halt.
The rootfs panic-loader service has not run in this incoming kernel.

For automatic warm recovery, the **incoming initramfs** must carry everything
needed to arm its own crash slot **before releasing the PCIe host gate**:
an appropriate capture Image, `kexec` and libraries, capture command line,
and self-contained phoenix initramfs embedding the immutable known-good
stage-2 Image/initrd/DTB/cmdline. It cannot depend on `/boot` or any disk reads.
Load with `kexec -s -p`, require `/sys/kernel/kexec_crash_loaded=1`, then release
storage probing while leaving the software deadline armed. The expected chain
is deadline panic → capture/host-only ANS quiesce → known-good full kernel;
the capture disables the deadline and never mounts NVMe. Each full incoming
kernel must rearm its own crash image. This follows the
[kernel kdump loading contract](https://cdn.kernel.org/doc/html/latest/admin-guide/kdump/kdump.html).
That early packaging/arming is **not implemented by this patch**.

Alternatively an initramfs recovery supervisor can load a normal self-contained
recovery entry before storage probing and invoke the established kexec path
on root timeout; that also requires packaging and testing, not merely enabling
the watchdog. `panic=N` with `apple_wdt.cold_restart=0` cannot select known-good;
allowing the hardware reset would return through iBoot/iOS and the tethered
cold boot chain. A software timer cannot guarantee recovery from IRQ-disabled
or fabric-wide hard lockup. Even phoenix may not repair a persistently stuck
S3E link, so a known-good image is not a guarantee of a successful warm boot.

## Files owned by this work

Workspace changes, all left uncommitted:

- `docs/boot/kexec-ans-powered-reset-2026-10-25.md` (this report).
- `scripts/boot/analyze-kexec-ans.py` (offline evidence comparison).
- `scripts/boot/test-ans-keep-power-offline.py` (new production-C tests).
- `scripts/boot/tests/ans-handoff-port.c` (test-only option/warm marker/reset stubs).
- `scripts/boot/test-ans-handoff-offline.py` (one additional helper extraction
  mapping; this file already had unrelated uncommitted changes, preserved).
- `patches/boot/kernel/0020-PCI-apple-h9p-add-an-opt-in-powered-ANS-kexec-reset.patch`.

Kernel: only `drivers/pci/controller/pcie-apple-h9p.c`, committed on
`a9/kexecfix` as above; branch clean. No configuration change.

Generated local files are confined to `build/boot/kexecfix-research/` (inspection
source copies, extracted current-storage logs, `analysis/*`, read-only parent
`baseline/*`, implementation/commit scratch files and test/build logs) and
Docker's existing `kexecfix` object/build outputs. No other existing edits were
reverted or committed.
