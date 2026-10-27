# Warm Linux reboots on j71t

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


A boot that comes up and then crawls (NVMe `completion polled`, frozen
`nvme0q` IRQ count) is a lost shared MSI, not a handoff failure. See
[ds-kexec-nvme-crawl.md](ds-kexec-nvme-crawl.md). `kexec-host.sh` now runs
`kexec-health-gate.py` after every warm boot and exits 3 on a crawling one.

For the 2026-10-24/25 tri43/tri46 failures, see the
[serial evidence, diagnosis and read-only collector](kexec-hang-2026-10-25.md).
The captured incoming boot reaches NVMe-root rescue; its later all-ones DART
error is a Wi-Fi bus/DART mismatch after ANS port 0 failed. The narrow
`a9/kexecfix` guard prevents that secondary attachment; ANS-link recovery is
still unresolved. The tri46 boot-deadline option was not built into its config.

Status: Round 4 same-entry/different-Image boots and SysRq recovery passed.
Two intermittent warm hangs were later reported; the captured incoming hang
ends at SPI1/audio controller registration, after `/init` starts.
[Round 6](ans-kexec.md#round-6--spi-handoff-retained-gpio-irqs-and-bounded-boot-evidence)
adds SPI shutdown/normalization, retained GPIO IRQ cleanup, probe milestones
and an opt-in software boot/handoff deadline. Offline validation is recorded
there; device validation is pending. The first device gate remains a cold boot
of the saved, still untested Round 5b pstore-fix payload. No device access was
performed by the implementation agent.

## Design

The initial boot is still palera1n/checkm8 -> pongoOS -> m1n1 -> Linux.
Subsequent reboots use Linux kexec, with Image, pristine DTB and initramfs stored
on the NVMe root under `/boot/linux-a9/entries/NAME`. No Mac is involved in a
normal warm reboot. Nothing installs a persistent bootloader or writes syscfg,
NVRAM, LLB or iBoot. A power loss or deliberate SoC reset still leaves Linux.

A9 has no EL2 and no native PSCI. The inherited m1n1 patch in
`patches/boot/m1n1/` supplies a small **opt-in resident EL3 shim**, selected by the
payload variable `psci=1`. This requires pongoOS to enter m1n1 at EL3; the first
boot must demonstrate `/psci/method=smc` and a `psci` CPU enable-method. If it was
entered at EL1, the patch leaves the spin table in place and the loader refuses
SMP kexec. Merely offlining CPU1 or passing `maxcpus=1` to a spin-table kernel is
insufficient: arm64 checks possible CPUs and the availability of `cpu_die`.
A cold boot with `nr_cpus=1` is the diagnostic alternative, with one CPU only.

The shim supports VERSION, FEATURES, CPU_ON, CPU_OFF, AFFINITY_INFO and
MIGRATE_INFO_TYPE. CPU_OFF cleans the private L1D and parks at EL3 in WFE; it does
not power off the core. CPU_ON hands it a clean EL1 entry with MMU/caches off.
Mailboxes, vectors and static stacks remain in m1n1's reserved memory. The normal
kernel's PSCI hotplug path can therefore park secondary CPUs before kexec and
restart them in the next kernel. SYSTEM_RESET/OFF are unsupported and return to
Linux's fallback handlers. This is specific firmware code requiring device
validation, not a claim of general PSCI compliance. The [Arm PSCI specification](https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca)
defines the CPU lifecycle/cache requirements used for the audit.

`linux-a9-kexec load NAME` uses `kexec_file_load` (`kexec -s -l`) when the DTB is
unchanged. The kernel copies its actual FDT, retaining m1n1's memory, reserved
regions, framebuffer, MAC/calibration, CPU and PSCI fixups. Never pass a pristine
DTS build directly to kexec: it lacks those runtime values.

For a changed DTB, `linux-a9-fdt-merge` applies `(running FDT - running pristine
DTB)` to the new pristine DTB. Phandle references are normalized by node path before comparison, preserving
provider arguments. Conflicting edits, lost fixed-up nodes, duplicate handles,
dangling references and unsupported reference layouts are refused. It keeps firmware
reservations, adds new static reservations, strips stale initrd/crash/seeding
properties, and marks `/chosen/linux,booted-from-kexec`. That path uses the
legacy syscall explicitly (`kexec -c -l --dtb=...`). Its correctness needs the
changed-DTB device test. Missing running provenance is an error. Supplying no
DTB means the new kernel is compatible with the running tree.

Entries are immutable and checksummed at staging and before load; a replacement with different contents is
refused. `default` and `previous` are atomically replaced symlinks. A plain
`reboot` reaches the `systemd-reboot.service` override, which uses a loaded image
or loads `default`, then tries `previous` if loading failed. This is a fallback
for **load** failures; a kernel that hangs after the jump cannot roll itself
back. The explicit `linux-a9-kexec reboot NAME` loads before stopping services
and enters `systemctl kexec`. An unsuccessful load returns to the live system.
The installation expects `/boot/linux-a9` on the NVMe root filesystem, not a
separate filesystem that shutdown unmounts before the reboot hook.

`apple_wdt.cold_restart=0` refuses a late SoC-reset fallback. The loader also sets
the guard in the currently running kernel. `panic=0` prevents an unhandled panic
from timing out into iOS. Systemd's runtime/reboot/kexec watchdogs are disabled;
this does not stop a watchdog owned by another process or firmware. If kexec
cannot proceed, halt and recover using the buttons and DFU. An explicitly
requested `linux-a9-kexec ios --yes` unloads the normal kexec image and allows the
watchdog restart handler to reset the SoC. It needs the Mac/checkm8 to return.

## Panic recovery

`crashkernel=256M` reserves a capture kernel. The panic service packages the
**running entry's** matching Image, a small initramfs, and the default entry's
full Image/initramfs/cmdline. `kexec -s -p` arms this separately from normal kexec.
The capture kernel uses `nr_cpus=1 cma=0 reset_devices rdinit=/init`, removes the
NVMe write window and root-mount arguments, and never mounts the NVMe root.
Phoenix now reprobes the host with ANS held, disables the unbound controller
and isolates port 0, then kexecs the full default with all RAM again. See the
Round 5 quiesce timeouts and legacy-capture compatibility rule.
Stage2's command line comes from the original full system, not the capture
kernel's one-CPU command line. A new full boot rearms the service.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Build and install

All build commands below are host-only. The current boot payload uses `TREE=integ`
and includes all nine workstreams. Include `a9/boot` explicitly so its config
fragment participates. Never put source edits on the generated `a9/integ` tree.

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

**Orchestrator only:** cold boot this new payload once, following the existing
DFU/pongo workflow. This is required to install the resident shim and reservation;
kexec from an old spin-table boot cannot retroactively install EL3 firmware.

```sh
scripts/boot.sh pongo
scripts/boot.sh linux build/boot/m1n1-linux-boot.bin
# If pongoOS ate the first bootm, use its documented second-bootm procedure.
scripts/omarchy/ssh.sh 'uname -a; cat /proc/cmdline; cat /sys/devices/system/cpu/{possible,online}; tr "\0" "\n" </proc/device-tree/psci/compatible; tr -d "\0" </proc/device-tree/psci/method; echo'
```

For offline package installation, fetch the required binaries and dependencies:

```sh
scripts/omarchy/fetch-live-pkgs.sh --out build/boot/pkgs kexec-tools cpio python gzip util-linux coreutils glibc
```

Then install and stage the **exact** cold-boot build. Default entry name matches
what the payload printed. Do not rebuild between producing the payload and
staging this entry.

```sh
scripts/boot/install-device.sh
TREE=integ scripts/boot/stage-kernel.sh --default \
  --cmdline "$(cat build/boot/integ-entry/cmdline)"
scripts/boot/install-device.sh --check
scripts/omarchy/ssh.sh 'findmnt /; findmnt -T /boot/linux-a9; sudo systemctl status linux-a9-kexec-panic.service --no-pager; cat /sys/module/apple_wdt/parameters/cold_restart; cat /sys/kernel/kexec_crash_loaded; systemctl show -p RuntimeWatchdogUSec -p RebootWatchdogUSec -p KExecWatchdogUSec; cat /sys/class/watchdog/watchdog0/state'
```

Expect NVMe root, the correct `linux_a9.entry`, both CPUs online, PSCI SMC,
watchdog inactive, cold_restart=N and crash_loaded=1. The installer creates the
config only if absent. If round 1 already installed a 192M config, update
`/etc/linux-a9/kexec.conf` to match the cold boot's 256M reservation. It enables
the panic service, sets runtime/persistent kernel.panic=0, and installs Omarchy
menu labels/actions. Verify menu behavior separately from the CLI path.

## Ranked orchestrator device tests

> Private graphics-dependent excerpt omitted from this source snapshot.

   ```sh
   scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -r; cat /proc/cmdline; sudo linux-a9-kexec load default; cat /sys/kernel/kexec_loaded'
   # Only after the read/load checks above pass:
   scripts/omarchy/ssh.sh 'sync; sudo systemctl kexec' || true
   # After the USB gadget re-enumerates:
   scripts/omarchy/ssh.sh 'uname -a; cat /proc/sys/kernel/random/boot_id; cat /sys/devices/system/cpu/{possible,online}; findmnt /; sudo dmesg | grep -Ei "psci|CPU1|kexec|nvme|pcie|dwc|dart|error|timeout"'
   ```

   Require a **different boot ID**, unchanged expected kernel, both CPUs online,
   NVMe root writable inside its existing write window, and SSH back. A disconnected
   SSH client alone is not evidence of success. Stop on PCIe/USB/DART errors.

2. **Prove CPU park/restart and plain reboot policy, with the same default.**

   ```sh
   scripts/omarchy/ssh.sh 'sudo sh -c "echo 0 > /sys/devices/system/cpu/cpu1/online"; cat /sys/devices/system/cpu/online; sudo sh -c "echo 1 > /sys/devices/system/cpu/cpu1/online"; cat /sys/devices/system/cpu/online; sudo dmesg | tail -50'
   scripts/omarchy/ssh.sh 'sudo kexec -u; sync; sudo reboot' || true
   # After reconnect, require a changed boot_id and both CPUs again:
   scripts/omarchy/ssh.sh 'linux-a9-kexec status; cat /proc/sys/kernel/random/boot_id; findmnt /'
   ```

   Unloading before `reboot` forces the installed dispatch hook to load default,
   instead of relying on systemctl's automatic jump to an already loaded image.
   Repeat 3–5 times. Then unplug the Mac and use Omarchy's “Reboot Linux (warm)”;
   require the local desktop, keyboard/touch and NVMe root to return. This is the
   proof that ordinary Linux -> Linux needs no host.

3. **Exercise the host one-command path, initially with that same artifact.**

   ```sh
   scripts/boot/kexec-host.sh build/boot/m1n1-linux-boot.bin
   # Image-only: reuse the running DTB/initramfs, asserting DT compatibility.
   scripts/boot/kexec-host.sh build/kernel-integ/Image.gz
   # A later build with a supplied DTB (and optional full cmdline):
   scripts/boot/kexec-host.sh /absolute/path/new/Image.gz /absolute/path/new/s8003-j71t.dtb
   ```

> Private graphics-dependent excerpt omitted from this source snapshot.

   Test a harmless changed-DTB model string in a separate artifact before relying
   on three-way DT merging. Check the new `/proc/device-tree/model`, PSCI, memory
   reservations, MACs and NVMe root. A merge refusal needs a cold m1n1 boot; do not
   bypass it with a pristine `--dtb`.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

   ```sh
   scripts/omarchy/ssh.sh 'sudo linux-a9-kexec panic-load; cat /sys/kernel/kexec_crash_loaded; cat /sys/kernel/kexec_crash_size; sync'
   scripts/omarchy/ssh.sh 'sudo taskset -c 0 sh -c "echo c > /proc/sysrq-trigger"' || true
   # After capture-kernel -> phoenix -> full-kernel boot:
   scripts/omarchy/ssh.sh 'uname -a; cat /proc/sys/kernel/random/boot_id; cat /sys/devices/system/cpu/{possible,online}; findmnt /; cat /sys/kernel/kexec_crash_loaded'
   ```

> Private graphics-dependent excerpt omitted from this source snapshot.

6. **Failure and cold-exit tests last.** `linux-a9-kexec load nonexistent` must
   return nonzero while the desktop/SSH stays alive. Test a deliberately conflicting
   DTB the same way. Do not corrupt the good/default entry. A late jump hang should
   stay halted (no watchdog/iOS reset); buttons must still force DFU. Finally:

   ```sh
   scripts/omarchy/ssh.sh 'sync; sudo linux-a9-kexec ios --yes' || true
   ```

   Expect iOS/cold boot, then recover Linux with DFU and the saved payload.
   This is intentionally the last test because it needs the host boot chain.

> Private graphics section omitted from this source snapshot.

## Recovery, rollback and optional helpers

- Before shutdown: a load, provenance, SMP, checksum or DT conflict failure aborts
  with an error. Fix/rebuild/stage with a new name while Linux is still usable.
- After shutdown: missing default and previous cause halt; a failed late kexec
  may also halt through the watchdog guard. Force-restart with the iPad buttons,
  hold Power + Home for DFU (release Power while continuing Home at the DFU stage),
  then run `scripts/boot.sh pongo` and `scripts/boot.sh linux <saved-payload>`.
  DFU is a physical recovery path; Linux cannot promise to enter it automatically.
- To choose a known-good kernel while Linux works:
  `sudo linux-a9-kexec reboot previous`. `set-default previous` swaps the default
  selection and rearms panic recovery. This does not repair a nonbooting kernel
  after the jump; retain a cold payload and its matching artifacts on the Mac.
- `scripts/boot/autoboot.sh` and `install-autoboot.sh` are optional Mac helpers,
  not part of Mac-free kexec. They watch for physical DFU and run the cold chain.
  Installing is explicit; set `build/boot/default-payload` to a tested payload
  symlink first. `touch build/boot/autoboot.disabled` pauses it. The implementation
  agent did not install a LaunchAgent or run any USB command.
- The inherited `m1n1-dev.sh`, trampoline and resident re-entry are experimental
  developer helpers. They rebuild m1n1's EL1 state and USB proxy and need a separate
  device test. They are unnecessary for the normal warm path and cannot replace
  resident EL3 code with a new firmware image. Do not use them as the first test.
- There is no automatic known-good rollback timer or persistent boot counter.
  Removing a default should not be the failure test on a machine you depend on.
  For new kernels use [kernel-updates.md](kernel-updates.md): `linux-a9-update
  boot` tries an entry once and only a health-checked boot of it changes the
  default; a hang before userspace still needs the Mac.
  Never stop the shared qemu-nbd root server by process name during any recovery.

## Original eight-branch offline evidence

> Private graphics-dependent excerpt omitted from this source snapshot.


## Nine-branch integration follow-up

The PMGR hook commit was amended to `72742531d3a9a625f5e58be595dafaf6b0a8d55b`.
It now runs immediately before genpd initialization and updates the captured
active state after a successful cycle. This avoids the overlapping insertion
with idle-floor setup; no other workstream branch was edited.

> Private graphics-dependent excerpt omitted from this source snapshot.

That payload was used for device test 1 and is superseded by the
[ANS follow-up build and exact staging commands](ans-kexec.md). Both the outgoing
and incoming kernels need the new shutdown code: cold boot the replacement
payload before testing same-kernel kexec again.

## Kexec into a kernel with a different DTB (2026-10-24)

This works if the running cold boot's own entry is staged. linux-a9-kexec needs that entry's pristine DTB to merge m1n1's properties into the target DTB. Without it, it refuses up front ("running pristine dtb unknown ... cannot prepare FDT"), and nothing runs. After a DFU cold boot of a payload, stage it:

    python3 -B -I scripts/boot/prepare-entry.py <payload.bin> <dir> --board apple,j71t
    scripts/boot/upload-entry.sh <entry name from linux_a9.entry=> <dir>      # not --default
    scripts/boot/upload-entry.sh <target entry> <target dir> --default

First success: nextint 92b11076f3-ring → tri36 a93e96bd8d (71 m1n1 properties merged), then three clean same-DTB ladder kexecs.
