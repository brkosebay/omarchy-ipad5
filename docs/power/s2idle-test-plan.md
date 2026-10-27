# J71t s2idle round 1: implementation and orchestrator test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. **Offline implementation; no device access or hardware results.**
The six incremental patches and branch bases/heads are in
[`patches/power/s2idle-round1/manifest.json`](../../patches/power/s2idle-round1/manifest.json).
No workspace commit, integration commit, push, default-entry change or device
operation is part of the build.

## What changed

- **a9/pcie:** D2365 `pmic@3c` describes AP GPIO **76**, level-low, with
  `interrupt-parent = <&pinctrl_ap>`, `wakeup-source`, `interrupt-controller`
  and one interrupt cell. Bindings describe these properties. CPU perf's
  AIC 76/79 remain unchanged. Power/Home already wake through AP GPIO 97/96.
- **a9/boot:** J71t system suspend shares round-8f's controller stop and
  port-down code. Drain → SHN/SHST → clear EN/SHN → RDY=0 (2 s) → CC/CSTS
  validation → BME off → DART inactive → fresh endpoint certificate → host
  PERST/LTSSM stop (250 ms) → RC124/114/100 bit 0 clear. NVMMU drain retains
  its 20 ms bound. SHN uses the existing `shutdown_timeout`; normal request
  draining uses the existing finite freeze timeout. Kexec retains its panic
  policy; sleep returns `-EBUSY`. An NVMe callback that fails queues its own
  controller restore, because PM does not resume that failed callback. A
  failed host noirq callback invokes the existing link restore path itself.
- **Pairing policy:** mode 0 still refuses sleep. On J71t, **modes 1 and 2 now
  cycle ANS port 0 too**; mode 2 also cycles Wi-Fi as before. Mode 3 still
  selects every port. Every ANS cycle requires this attempt's stop
  certificate. There is no endpoint BAR access in the host certificate check.
  Normal resume reuses full port setup, PCI state restore, 100 ms settle,
  NVMMU reprogramming and the existing checked DART/controller/health gate.
  NVMMU allocations remain valid across system sleep.
- **a9/pcie:** S8000 DWI maps `0x20e200000 + 0x4000`. J71t probe and parent
  resume replay, in order, `(0x00,1)`, `(0xd0,0x2ee00)`, `(0x04,0x78)`,
  `(0x84,0)`, `(0x80,0x41)`. Probe preserves the inherited brightness. The
  backlight class already restores brightness after parent resume. Busy
  checks are bounded at 1 ms and return/log errors. Legacy short mappings
  retain their channel addressing. The existing brightness control word's
  bit 12 is unchanged; other boards do not receive J71t's boot constants.
- **a9/touch:** mask/synchronize IRQ and cancel deferred recovery before the
  SPI parent can suspend; retain N1 rails, firmware, calibration and reset.
  Resume sends the runtime `ee...` wake, drains at most eight frames, checks
  ATN, clears the frame error count and balances the single IRQ mask depth.
  One runtime discovery plus another bounded drain is the fallback. Deferred
  recovery waits one second and allows at most three attempts per boot.
  Suspend and removal cancel it. A new resume may rearm even after that
  worker budget is exhausted. No per-frame wake packet or SPI register cache
  is added; SPI already reprograms its registers on every transfer.

**PMU limitation:** this round describes the hardware only. The current simple
MFD driver does **not** request/acknowledge GPIO76 or register a PMU irqchip;
RTC still has no alarm operations/event index. Charger/RTC/cover wake is
therefore **not enabled** by these DT changes. No child event specifier or
unknown PMU write was added. Do not run `rtcwake` as a round-1 acceptance test.
See [wake-plan §1.8](ds-suspend-wake-plan.md#18-the-dts-diff), including its
correction of the older study's CPU-PMU misreading.

## Build and reproduce, offline

Run from `.`:

```sh
scripts/power/export-s2idle-round1.sh
KERNEL_JOBS=1 KERNEL_MEMORY=1536m scripts/build-kernel.sh --tree boot \
  > build/power/s2idle-round1/boot-build.log 2>&1
scripts/power/build-s2idle-round1.sh \
  > build/power/s2idle-round1/integration-build.log 2>&1
scripts/power/test-s2idle-round1.sh \
  > build/power/s2idle-round1/sleep-tests.log 2>&1
scripts/boot/test-ans-handoff-offline.sh \
  > build/power/s2idle-round1/handoff-tests.log 2>&1
scripts/power/test-s2idle-round1.sh --boot /src/wt/s2idle-check \
  --touch /src/wt/s2idle-check --pcie /src/wt/s2idle-check \
  > build/power/s2idle-round1/sleep-integration-tests.log 2>&1
scripts/boot/test-ans-handoff-offline.sh --source /src/wt/s2idle-check \
  > build/power/s2idle-round1/handoff-integration-tests.log 2>&1
scripts/build-kernel.sh --run s2idle-check \
  'python3 -B -I /work/scripts/power/check-s2idle-round1-dtb.py arch/arm64/boot/dts/apple/s8003-j71t.dtb'
scripts/boot/test-fdt-merge.sh build/boot/round8f/entry-handoff/dtb \
  build/kernel-s2idle-check/dtbs/s8003-j71t.dtb \
  > build/power/s2idle-round1/fdt-tests.log 2>&1
python3 -B -I scripts/power/package-s2idle-round1.py
shasum -a 256 -c build/power/s2idle-round1/SHA256SUMS
```

> Private graphics-dependent excerpt omitted from this source snapshot.

Offline checks passed: five sleep fault-injection suites and 23 existing
handoff tests on both owner sources and the detached integration, 14 FDT
merge tests, compiled-DTB assertions, and full owner-tree reconstruction
from the six exports. Full LLVM arm64 builds passed: 104 standalone boot
DTBs and 105 integration DTBs, with `Image.gz` for each. The remaining full
integration build resumed with two jobs under the same 1536 MiB container
cap after checking VM memory availability. Strict exported-patch checkpatch reports zero errors,
warnings or checks. W=1 reports the existing `simple_bus_reg` warning for
`/soc/bus@20c000000` (expected unit address `20c900000`); these patches do not
change that node. Hardware validation remains pending.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Orchestrator: stage and warm kexec

**Everything below accesses the device. Builder ran none of it.** Start from
a healthy NVMe-root boot with both CPUs, both links and USB/SSH working. Keep
the known-good round-8f entry and default available. Preserve the running root
server if another session uses NBD; never kill QEMU/qemu-nbd by name.

First, in a separate Mac terminal, capture raw netconsole continuously:

```sh
scripts/omarchy/netconsole.py build/power/s2idle-round1/device-netconsole.log
```

In the task terminal:

```sh
shasum -a 256 -c build/power/s2idle-round1/SHA256SUMS
ENTRY=$(cat build/power/s2idle-round1/entry-name.txt)
scripts/boot/install-device.sh --check
scripts/boot/upload-entry.sh "$ENTRY" build/power/s2idle-round1/entry
scripts/omarchy/ssh.sh 'readlink /boot/linux-a9/default; cat /proc/cmdline'
scripts/boot/warm-chain.sh "$ENTRY" 1 build/power/s2idle-round1/device/warm-entry
scripts/omarchy/ssh.sh 'uname -r; cat /proc/cmdline; nproc; findmnt -no SOURCE,OPTIONS /; cat /sys/class/nvme/nvme0/state'
scripts/omarchy/ssh.sh 'sudo dmesg | grep -E "incoming port 0|full-bringup-done|DART checked reinit|health gate|DWI block initialized|N1 ready"'
```

Require warm-chain PASS, release containing `-a9-s2idle-r1-`, the staged
entry identity, two CPUs, `/dev/nvme0n1p2` RW and `nvme0 live`. Require the
round-8f ordinary full bring-up, DART reinit and health result 0. Raw outgoing
netconsole must show SHST, `STEP shutdown disabled ... BME=0` and all three
port-off bits clear. A recovery ladder success is a failed reliability sample.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.


The shipped rootfs provides these user services. Stopping the ALS service
preserves its saved on/off preference. The helper sets the temporary
policy knobs by hand, keeps the USB gadget, and restores their previous values on
exit. It also sets the existing `sleeping`/`resumed` guard around direct sysfs
sleep, preventing the wake press from immediately causing another sleep.

Identify the two key IRQs without letting the normal Power binding sleep:

```sh
scripts/omarchy/ssh.sh 'sudo mkdir -p /run/linux-a9; sudo touch /run/linux-a9/sleeping; cat /proc/interrupts' \
  > build/power/s2idle-round1/device/key-before.txt
# Press Power once, then capture; press Home once, then capture again.
scripts/omarchy/ssh.sh 'cat /proc/interrupts' > build/power/s2idle-round1/device/key-after-power.txt
scripts/omarchy/ssh.sh 'cat /proc/interrupts' > build/power/s2idle-round1/device/key-after-home.txt
scripts/omarchy/ssh.sh 'sudo rm -f /run/linux-a9/sleeping'
```

## Orchestrator: refusal, PM tests, then Power/Home freeze and mem

Run **one command at a time**, wait for its result, collect its log and apply
the acceptance checks below before the next command. Start jobs with systemd
so the SSH client cannot interrupt a test. The helper writes the requested
`echo freeze > /sys/power/state` or `echo mem > /sys/power/state` internally;
`mem_sleep` is explicitly `s2idle`. Both paths exercise device PM.

```sh
# Expected refusal: no port-down marker, NVMe remains usable.
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=s2idle-r1-refuse /tmp/s2idle-round1.sh 0 devices freeze power'

# Automatic five-second return: first device callbacks, then noirq/port cycle.
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=s2idle-r1-devices /tmp/s2idle-round1.sh 2 devices freeze power'
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=s2idle-r1-platform /tmp/s2idle-round1.sh 2 platform freeze power'

# Real sleep: wait at least ten seconds, then press the named key once.
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=s2idle-r1-freeze-power /tmp/s2idle-round1.sh 2 none freeze power'
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=s2idle-r1-freeze-home /tmp/s2idle-round1.sh 2 none freeze home'
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=s2idle-r1-mem-power /tmp/s2idle-round1.sh 2 none mem power'
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=s2idle-r1-mem-home /tmp/s2idle-round1.sh 2 none mem home'
```

After **each** command, save evidence on the Mac, then check screen/touch:

```sh
mkdir -p build/power/s2idle-round1/device
scripts/omarchy/ssh.sh 'sudo cat /run/s2idle-r1/last.log' \
  > build/power/s2idle-round1/device/latest-cycle.log
scripts/omarchy/ssh.sh 'sudo tar -C /run -cf - s2idle-r1' \
  > build/power/s2idle-round1/device/all-cycles.tar
scripts/omarchy/ssh.sh 'sudo dmesg' > build/power/s2idle-round1/device/dmesg.txt
scripts/omarchy/ssh.sh 'cat /proc/interrupts; ip -br link; cat /sys/class/nvme/nvme0/state'
scripts/omarchy/ssh.sh 'for b in /sys/class/backlight/*; do cat "$b/brightness" "$b/actual_brightness" "$b/bl_power"; done'
# Start this, then tap, drag and lift two fingers for 15 seconds.
scripts/omarchy/ssh.sh 'for p in /sys/class/input/event*/device/name; do if [ "$(cat "$p")" = "Apple N1 Touchscreen" ]; then e=${p%/device/name}; sudo timeout 15 evtest "/dev/input/${e##*/}"; fi; done' \
  > build/power/s2idle-round1/device/touch-events.txt
```

Keep each log under a distinct local name if preserving multiple touch traces;
`all-cycles.tar` contains the timestamped kernel/I/O logs. A timeout exit from
`evtest` is expected; require actual ABS/BTN_TOUCH events and release events.

Acceptance for every real cycle:

1. `rc=0`, meaningful asleep duration, `PM: Triggering wakeup from IRQ ...`,
   and the pressed Power/Home line's IRQ count increased. Do not assume IRQ66
   always names the same key. Compare against the guarded key identification
   above. No immediate second suspend.
2. Outgoing `sleep stop certified` precedes `sleep port 0 fully down`;
   `STEP shutdown disabled` shows EN/SHN/RDY/CSTS cleared and BME off.
   RC124/114/100 all have bit 0 clear. The host never powers down port 0
   without that certificate. Incoming full bring-up, NVMMU reprogramming,
   DART rebuild and admin/I/O health result 0 complete normally.
3. Panel visibly returns at 40%, with brightness 819 and `bl_power=0`.
   `DWI block initialized` appears on resume; no busy timeout. Test another
   brightness change after wake. A lit panel with correct brightness is
   required; sysfs values alone are insufficient.
4. `N1 runtime wake/drain complete; IRQ rearmed`, increasing N1 IRQ count,
   two-finger movement and complete contact releases. No stuck finger or
   manual rebind. Repeat once holding a finger across the transition.
5. Helper PASS includes live NVMe, a bounded 64 MiB direct partition read,
   direct file checksum across sleep, and a post-resume filesystem fsync
   write. No timeout, quarantine, controller recovery or filesystem error.
6. USB stays enumerated on the Mac, the ECM link has carrier and SSH returns.
   The independent raw netconsole capture continues. Compare interface
   state/counters before and after; reconnecting the cable is a failure.

After the four pilot sleeps, repeat Power and Home until **ten real cycles**
have passed, alternating `freeze`/`mem`, with a tap between cycles. Give each
systemd unit a new suffix. Then test blanking separately: `echo 1` to the
backlight's `bl_power`, one real sleep, require it remains blank on wake;
write `0` to `bl_power` and require the saved brightness to appear. Also run
one sleep after brightness 0, then explicitly restore 819 and require light.
Mode 3 is an additional cohort after all these pass; use the same helper
with mode 3, new unit names and the same per-cycle checks.

## Failure handling and limits

Stop at the first missing certificate, failed STEP, SHN/RDY timeout, NVMMU/
DART error, reset/recovery, dark screen, touch rebind, root or USB failure.
Save continuous netconsole, logs and both pstore locations before rescue:

```sh
scripts/omarchy/ssh.sh 'sudo linux-a9-pstore --read; sudo ls -l /sys/fs/pstore'
```

> Private graphics-dependent excerpt omitted from this source snapshot.

The warm-entry command line retains `apple_pmgr.kexec_keep_on=1`. On a warm
boot this inhibits genpd power-off, so this cohort verifies replay/order and
the explicit PCIe port cycle, **not recovery from a proven DWI/SPI domain
power loss**. It does not validate deeper CPU sleep, display-pipe shutdown,
charger/RTC wake, the inferred level-low PMU trigger, or analog PCIe recovery.
Those require separate measured cohorts. Software poll bounds also cannot
interrupt a CPU-stalling hardware MMIO transaction.

When done, restore brightness/blanking and restart the user services:

```sh
scripts/omarchy/ssh.sh 'sudo sh -c '\''for b in /sys/class/backlight/*; do echo 0 > "$b/bl_power"; echo 819 > "$b/brightness"; done'\''; systemctl --user start hypridle.service ipad-als-brightness.service'
```

Keep the new entry staged for review; leave the default unchanged.
