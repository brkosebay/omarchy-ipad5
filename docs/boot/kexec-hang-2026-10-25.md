# tri43/tri46 intermittent warm-kexec rescue: research and collection

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Research agent, 2026-10-25. No device access. No workspace commits or pushes.
Kernel-only commit: `a9/kexecfix`, `6d449cccb367476701872e683021be769c1f5dca`,
based on the exact tri46 `4dd43c040f` commit.

## Finding

> Private graphics-dependent excerpt omitted from this source snapshot.

The exact reason the ANS link intermittently fails is **not established**.
The supplied three ladder logs contain no failed incoming dmesg. The fourth
capture localizes the failure but also reveals a secondary routing bug that
must not be mistaken for its cause.

### The all-ones DART is a secondary Wi-Fi misattachment

Read the un-replayed [36.52–37.2 s trace](../release/OMISSIONS.md):

1. Port **1** comes up. This is Wi-Fi, not the NVMe/ANS port **0**.
2. The driver prints `rescue-bus secondary=2 (fixed DART RID)`.
3. Generic PCI scanning nevertheless publishes **14e4:43dc Wi-Fi at
   `0000:01:00.0`**, and later prints `PCI bridge to [bus 01]`.
4. It attaches that device to **`601008000.iommu` (ANS DART0)**, whose TCR,
   TTBR and error registers read all ones. Attach fails with `-EIO`, producing
   the `iommu_setup_default_domain` warning from `apple_h9p_late_ports_work`.

`arch/arm64/boot/dts/apple/s8003.dtsi` has the fixed mapping
`0x100 -> pcie0_dart0`, `0x200 -> pcie0_dart1`, masked with `0xff00`.
Wi-Fi should attach to `602008000.iommu`. The generic
`pci_host_common_init()` sets **`PCI_REASSIGN_ALL_BUS`** before
`pci_host_probe()`. In `pci_scan_bridge_extend()`, that flag overrides the
preprogrammed secondary bus and assigns `max + 1`. If ANS is absent, Wi-Fi
therefore takes bus 1. The old rescue-bus fixture tested the register store,
not whether generic PCI scanning would honor it.

> Private graphics-dependent excerpt omitted from this source snapshot.

The [`PMGR` capture](../release/OMISSIONS.md) reports
`warm=1 hold=1` for PCIe domains and active readbacks for core/ref/aux.
That is inconsistent with claiming that `kexec_keep_on` was simply ignored.
It does not prove every clock, reset or analog rail is correct. Do not toggle
the shared PCIe/DART domain on this evidence: it can disturb other consumers.

### Additional evidence and boot-path checks

> Private graphics-dependent excerpt omitted from this source snapshot.

## Changes

1. [`collect-hang-evidence.sh`](../../scripts/boot/collect-hang-evidence.sh)
   and its small [`tty-readonly.py`](../../scripts/boot/tty-readonly.py)
   helper collect through SSH, initramfs telnet, or an explicit Mac ACM port.
   All device queries are read-only. No mounts, pstore deletions, sysfs stores,
   endpoint MMIO, block reads, retries, service changes or reboot commands.
   Host output includes USB/network state, full current dmesg, `/HL_init.log`,
   two IRQ snapshots, root/PID1/PCI/NVMe/PMGR state, service/journal status,
   and pstore records from sysfs, initramfs copies and normal-root archives.
   Raw serial and partial captures remain available if a timeout interrupts
   the archive. Passive serial mode sends no commands and reconnects across
   ACM re-enumeration. Do not run two readers against the same serial port.
2. [`linux-a9-pstore`](../../scripts/boot/device/linux-a9-pstore) now copies
   records and prints only their paths. Previously its `cat` fed the old
   persistent console through the initramfs logger into the new persistent
   console. Successful r46 logs contain many generations of nested replay;
   that consumes the 1 MiB console ring and delays early diagnostics.
   This is a proven logging bug, not a proven cause of link failure. Both
   panic and console records remain intact and archivable.
3. [`prepare-hang-diag-entry.py`](../../scripts/boot/prepare-hang-diag-entry.py)
   prepares a separate immutable entry from an existing local one. It overlays
   only the pstore reader, preserving kernel, DTB, `/init`, firmware and rescue
   hooks byte-for-byte. The only cmdline change is entry identity. Already
   prepared locally: **`build/boot/tri46-hangdiag-entry`**, name
   **`tri46-hangdiag-1`**. This does not include the optional kernel guard.
4. Kernel commit **`6d449cccb3`** defers j71t trial-mode late ports while
   ANS port 0 is absent, before Wi-Fi power/train/enumeration. On a successful
   explicit ANS rescue, the existing ANS scan reserves bus 1, then late work
   is rescheduled for Wi-Fi/bus 2. Normal ANS-present boots, Round 7 and other
   boards keep their prior behavior. Export:
   [`0019-PCI-apple-h9p-defer-late-ports-while-ANS-is-missing.patch`](../../patches/boot/kernel/0019-PCI-apple-h9p-defer-late-ports-while-ANS-is-missing.patch).
   **This guards the wrong-DART attachment; it does not fix the missing ANS
   link.** It makes no PMGR, clock, DART-register or link-training changes.

## Orchestrator steps

### Capture the current hang before recovering

Use the existing root rescue prompt, without rebooting or retrying storage:

```sh
scripts/boot/collect-hang-evidence.sh --transport serial --serial auto \
  --seconds 120 --out build/boot/hang-evidence-r46c-full
```

`auto` requires exactly one `/dev/cu.usbmodem*`; otherwise pass the known
device path. The default transport tries SSH then initramfs telnet. To use
serial explicitly after SSH has failed, use the command above. Missing
commands such as systemctl in BusyBox are recorded as errors and skipped.
The output directory must be new. Verify `result.txt`, `device.txt`,
`serial-raw.txt` and `records/`; retain partial output too. The collector
opens an existing shell, not a login sequence. It never retries a command
after an active serial disconnect.

The initramfs serial shell is independent of `systemd.debug_shell=ttyGS0`:
systemd is not running yet. Once the real root starts, that cmdline enables
the systemd debug shell on the same ACM function. Standard kernel
[ACM gadget documentation](https://docs.kernel.org/usb/gadget_serial.html)
and [systemd debug-generator documentation](https://www.freedesktop.org/software/systemd/man/latest/systemd-debug-generator.html)
describe the two mechanisms.

### Deploy the userspace diagnostics without changing the kernel

After normal recovery, first collect the healthy state/pstore:

```sh
scripts/boot/collect-hang-evidence.sh --transport ssh \
  --out build/boot/hang-evidence-after-recovery
scripts/boot/upload-entry.sh tri46-hangdiag-1 build/boot/tri46-hangdiag-entry
```

Leave the default entry unchanged. On the device, through the orchestrator:

```sh
sudo linux-a9-kexec load tri46-hangdiag-1
sync
sudo systemctl kexec
```

Before jumping, run passive serial collection in a second host terminal:

```sh
scripts/boot/collect-hang-evidence.sh --transport serial --serial auto \
  --passive --seconds 180 --out build/boot/hang-evidence-next-boot-stream
```

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

To prepare the same diagnostics for a later entry:

```sh
python3 -B scripts/boot/prepare-hang-diag-entry.py LOCAL_ENTRY NEW_LOCAL_ENTRY \
  --name NEW_IMMUTABLE_NAME
```

### Optional kernel guard validation

> Private graphics-dependent excerpt omitted from this source snapshot.

On a healthy boot, require ANS at bus 1/DART0 and Wi-Fi at bus 2/DART1.
On the next natural ANS-link failure, require:

- `ANS port 0 missing; deferring late ports ... to preserve DART routing`;
- no Wi-Fi `14e4:43dc` function at `0000:01:00.0`;
- no resulting Wi-Fi attach against `601008000.iommu`;
- usable ACM/ECM rescue and full current port-0 failure evidence.

The collector performs no rescue retry. If the orchestrator separately
chooses the existing explicit ANS retry, a successful ANS publication must
precede Wi-Fi late work and leave the same bus/DART identities. Do not call
a recovery-assisted sample a passing clean warm boot.

## Offline validation

- Four host collector tests passed: fake SSH archive extraction/read-only
  query contract; localhost telnet with split IAC negotiation; PTY serial
  multiline queries/archive extraction/terminal restoration; passive serial
  sends no input. No iPad connection was made.
- Two production-kernel regression tests passed on `kexecfix`, **both fail on
  unmodified tri46**: missing ANS blocks late-port work, and only a successful
  ANS rescan reschedules it. ASan/UBSan enabled.
- All 23 existing ANS handoff tests passed against `/src/wt/kexecfix`.
- All five pstore/phoenix recovery tests passed against `/src/wt/tri46`.
- ARM64 `drivers/pci/controller/pcie-apple-h9p.o` compiled using tri46's config;
  log: `build/kernel-kexecfix/compile.log`. Strict checkpatch: zero findings.
- The diagnostic entry builder checked every unmodified initramfs file,
  including `/init`, kernel and DTB preservation. Shell/Python syntax passed.

Reproduce the new suites with:

```sh
python3 -B scripts/boot/test-collect-hang-evidence.py
scripts/build-kernel.sh --run kexecfix \
  'python3 -B -I /work/scripts/boot/test-kexecfix-offline.py --source /src/wt/kexecfix'
scripts/boot/test-ans-handoff-offline.sh --source /src/wt/kexecfix
scripts/boot/test-recovery-offline.sh --source /src/wt/tri46
```

## Files changed by this research agent

Workspace files, left uncommitted:

- `docs/boot/kexec-hang-2026-10-25.md` (new report)
- `docs/boot/warm-reboot.md` (link and current findings)
- `scripts/boot/collect-hang-evidence.sh` (new collector)
- `scripts/boot/tty-readonly.py` (new serial helper)
- `scripts/boot/prepare-hang-diag-entry.py` (new local entry builder)
- `scripts/boot/device/linux-a9-pstore` (stop automatic record replay)
- `scripts/boot/test-collect-hang-evidence.py` (new host transport tests)
- `scripts/boot/test-kexecfix-offline.py` (new kernel regression tests)
- `scripts/boot/test-recovery-offline.py` (pstore preservation/no-replay test)
- `scripts/boot/tests/ans-rescue.c` (support deferred late-work scheduling)
- `patches/boot/kernel/0019-PCI-apple-h9p-defer-late-ports-while-ANS-is-missing.patch`
  (exported kernel commit)

Kernel branch `a9/kexecfix` only:
`drivers/pci/controller/pcie-apple-h9p.c` (17 added lines).
Generated local files: `build/boot/tri46-hangdiag-entry/*`,
`build/boot/hang-device-queries.sh`, `build/boot/kexecfix-commit-message.txt`,
`build/kernel-kexecfix/compile.log` and Docker worktree/build outputs.
All other pre-existing working-tree changes were left alone.
