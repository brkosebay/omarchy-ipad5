# A9 ANS shutdown and early USB follow-up

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


The orchestrator cold-booted integration `ecc62a5790`, installed/staged its exact
entry and successfully loaded it with `kexec_loaded=1`. At 02:13:20 on
2026-10-23, `systemctl kexec` entered the next kernel and printed to the
framebuffer. The owner read the last line as ANS controller enable with a
120-second RDY timeout / 100-ms SoC-side watch. USB never re-enumerated; the NVMe
root could not mount. The [host transcript](../release/OMISSIONS.md) ends at
the jump; the framebuffer observation is the owner's report, not a captured
kernel trace. This establishes entry into the new kernel, not successful warm
storage initialization or full SMP recovery.

The following fix is built and tested offline. Its hardware result is pending.
Only the orchestrator runs the device commands below.

## Why the fix uses PCIe shutdown

j71t uses `pci-apple-ans.c`, a PCI S3E endpoint behind `pcie-apple-h9p.c` with
NVMMU/HMB and the S5L8960X DART. It does not use the later platform `apple.c`
ANS driver. The j71t ADT has `apcie`, `nvme-mmu0` and `dart-apcie0`; there is no
matching `/arm-io/ans` ASC or `sart-ans` block to which m1n1's later-chip RTKit
`nvme_shutdown()` sequence could safely be applied. No ANS-specific PMGR domain,
RTKit sleep command or firmware upload is guessed here.

The existing generic PCI shutdown calls `nvme_disable_ctrl(..., true)`: it sends
normal SHN and waits for SHST, leaving CC.EN set. The H9P platform driver had no
shutdown callback, so the endpoint/link and old host mappings could survive the
jump. The missing complete shutdown is a concrete source-level gap; that it
caused this exact hang remains an inference awaiting the next test.

The [NVMe 1.3a specification](https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_3a-20171024_ratified.pdf)
distinguishes shutdown notification from controller reset through CC.EN. Existing
[iOS kernelcache findings](../pcie/enable-freeze.md#nvme-enable-path-same-registers-as-linux-verified)
show CC=0/RDY=0 before reprogramming queues on this endpoint. The H9P driver's
existing `apple_s8003_port_sleep()` implements the iOS-derived port-disable
sequence, including PME_TO/L2, PERST and
port power/refclk removal; see the
[PCIe findings](../pcie/kernelcache-notes.md#q3-port-bring-up-sequence-s8003).
These are the evidence for completing the outgoing shutdown instead of applying
a reset sequence from another SoC.

## Kernel changes on a9/boot

`a9/boot` merges the existing `a9/pcie` commit `2752fedbbf` as dependency merge
`bf14b32eaa`. No other branch ref was modified. Two separate boot-owned patches
follow that merge:

| Commit | Change |
| --- | --- |
| `76caa84ff2` | NVMe/ANS kexec shutdown and H9P port/SART teardown |
| `f645a3c0ae` | j71t ANS probe gate for early USB diagnostics |

For a normal kexec, the outgoing kernel now performs this sequence:

1. Keep the old mappings alive while the generic NVMe path freezes/drains I/O,
   deletes I/O queues and sends normal SHN. Log the SHN result.
2. In the ANS-specific callback, clear CC.EN and SHN and poll RDY=0 for at most
   two seconds. Use the existing link/fault guards; a known fault skips endpoint
   BAR access. A timeout is logged and continues to physical endpoint isolation.
3. Clear endpoint PCI bus mastering before the host is parked. PCI core also
   clears it during kexec shutdown.
4. After PCI children shut down, cancel H9P late-port work, block further config
   routing and use the existing bounded PME_TO/L2 sleep sequence. Assert PERST.
5. With the endpoint held in reset and port registers still powered, disable
   the owned NVMMU HMB/SART aperture. Then remove port power/refclk. Leave DART
   programming to its driver; the new kernel initializes its own translations
   and NVMMU state on bring-up. No live translation table is zeroed under DMA.

The new callbacks are gated by normal `kexec_in_progress`; the host callback
also checks `apple,s8003-pcie`. Ordinary reboot/suspend behavior is retained.
Panic kexec skips `device_shutdown()`, so this graceful sequence does **not**
establish that panic recovery can reset retained storage. Keep the panic test
behind successful ordinary kexec tests; a panic-time storage hang may still
require DFU. The existing cold-reset guard remains in force.

## USB diagnostics before ANS enable

`CONFIG_NVME_APPLE_ANS_EARLY_USB=y` makes `nvme.apple_ans_probe` default to false
on j71t. The NVMe PCI probe returns `-EPROBE_DEFER` before allocating queues or
touching ANS BARs. Other boards are unaffected. The boot-owned initramfs overlay
then performs the following before attempting NVMe root:

- Run the existing ECM/ACM gadget and USB address/DHCP setup, plus serial and
  telnet rescue shells.
- Verify a UDC is bound and `/dev/ttyGS0` exists. Start a background
  `/dev/kmsg` stream to ACM, including the new kernel's backlog. Attempt the
  existing netconsole setup for a bounded interval (default 10 iterations,
  no more than 30); absence of a Mac does not block boot indefinitely.
- Print `EARLY USB READY`, release the gate, and reprobe the unbound Apple
  storage BDF in a background process. PID1 and rescue shells remain runnable
  if the probe worker blocks. Stop the serial log forwarder before switch_root
  so the existing systemd debug shell can use ttyGS0.

The helper lives in `scripts/boot/device/early-storage.sh`; the builder is
`scripts/boot/build-initramfs.sh`. It packages the current NVMe-aware
`scripts/omarchy/initramfs/init`, because the cached base image contained an
older `/init`, and adds only the logging/gate helper. Every other base initramfs
file, including firmware and BusyBox, is preserved byte for byte. It leaves the
NVMe workstream's source and base image unchanged. All three boot entry points
(`mkpayload`, `stage-kernel`, `m1n1-dev`) default to this boot-owned image.

If the UDC or ACM device is missing, the helper leaves the gate closed and enters
rescue. A hang in earlier PCI host/DART initialization can still precede `/init`.
A fabric-wide hard lock can still stop USB after it is configured. Early logging
improves observability; it cannot guarantee delivery after such a lock. Outgoing
shutdown logs may also stop when USB's own shutdown callback runs.

## Build and offline evidence

Follow the [warm reboot build commands](warm-reboot.md#build-and-install), including
the boot initramfs builder and all nine branch names. Keep the five requested
extra boot arguments unchanged:

```sh
TREE=integ scripts/boot/mkpayload.sh \
  log_buf_len=32M apple_a9_dvfs_ro.enable=1 cfg80211.ieee80211_regdom=NL \
  systemd.debug_shell=ttyGS0 systemd.mask=maintainer@example.invalid
python3 -B -I scripts/boot/prepare-entry.py \
  build/boot/m1n1-linux-boot.bin build/boot/integ-entry
```

The build evidence is under `build/boot/`: `ans-integrate-check.log`,
`ans-build.log`, `ans-initramfs.log`, `ans-build.json`, `test-offline.log`,
`test-ans-offline.log`, `ans-check-build.log`, `ans-fdt.log` and `ans-payload.log`.
The manifest records the exact branch heads, artifact hashes and full command
line; generated build output is not committed.

> Private graphics-dependent excerpt omitted from this source snapshot.

Payload: `build/boot/m1n1-linux-boot.bin`, entry `integ-c13f4742cf`.
SHA-256: `c67b48c769915dc4a496689973fccbb1825ea12981cb01931ffe7bd85884f62a`.

The storage tests compile the actual production shutdown C with mock MMIO,
including success, link loss, host fault, CSTS error and timeout. They check
CC.EN clear / bus-master block / PERST / SART off / port power-off ordering.
Shell fixtures check logging before probe and that missing ACM or an unbound
UDC keeps storage gated. The real packed initramfs is checked for ordering,
preserved base contents and a single gzip stream. The general boot policy tests
also cover the first-default staging regression seen in the transcript and the
status check now ignores a comment when counting the reboot hook. These checks
validate software behavior, not the S3E's reset response on hardware.

## Next orchestrator run

Cold boot the replacement `build/boot/m1n1-linux-boot.bin` through DFU/pongo
first. **Both outgoing and incoming kernels must have the shutdown patch.**
Staging the new kernel while still running `ecc62a5790` cannot repair its missing
outgoing shutdown. After the new cold boot and before warm test 1, run the same
three staging commands on the Mac:

```sh
scripts/boot/install-device.sh
TREE=integ scripts/boot/stage-kernel.sh integ-c13f4742cf --default \
  --cmdline "$(cat build/boot/integ-entry/cmdline)"
scripts/boot/install-device.sh --check
```

Do not rebuild or change artifacts between this cold boot and staging. The
staged default is the exact cold-booted kernel, DTB and boot-owned initramfs.

Before warm reboot, start a host listener in another terminal:

```sh
scripts/omarchy/netconsole.py build/boot/ans-next-netconsole.log
```

Capture ACM as well if possible (the device node may change on re-enumeration).
On the next kernel it should deliver the backlog before ANS enable, then reach
`EARLY USB READY`, `reprobe PCI ANS`, the CC.EN message and either the new
`100ms after CC.EN ... polling CSTS` line or a fault. Do not interpret carrier
or an SSH disconnect as a successful reboot.

The agreed order is:

> Private graphics-dependent excerpt omitted from this source snapshot.

   ```sh
   scripts/omarchy/ssh.sh 'uname -a; cat /proc/sys/kernel/random/boot_id; findmnt /; cat /sys/module/nvme/parameters/apple_ans_probe; sudo linux-a9-kexec load default; cat /sys/kernel/kexec_loaded'
   scripts/omarchy/ssh.sh 'sync; sudo systemctl kexec' || true
   # Reconnect only after the new USB gadget or Wi-Fi returns:
   scripts/omarchy/ssh.sh 'uname -a; cat /proc/sys/kernel/random/boot_id; cat /sys/devices/system/cpu/{possible,online}; findmnt /; sudo dmesg | grep -Ei "EARLY USB|ANS|nvme|NVMMU|SART|DART|PCIe|timeout|error"'
   ```

> Private graphics-dependent excerpt omitted from this source snapshot.

If the gate remains closed in an initramfs rescue shell, inspect
`cat /config/usb_gadget/g1/UDC`, `ip link` and `/dev/ttyGS0`. After repairing
diagnostics, an explicit retry is:

```sh
linux_a9_early_storage
touch /run/omarchy-continue
```

If ANS still stalls, leave its register state alone and collect logs; do not
blindly poke PMGR, RTKit, SART or DART. The existing 120-second controller timeout
and initramfs root timeout are separate: PID1 may already have fallen back to
NBD/rescue while the controller worker is still waiting. The USB rescue shells
are available throughout unless the hardware stops the CPU/fabric.

## Separate ECM failure reported by the owner

On cold-booted `ecc62a5790`, ECM stopped passing packets about one minute after
boot while carrier stayed up on both sides. ACM continued working; restarting
dnsmasq and bouncing usb0 did not recover it; Wi-Fi SSH worked. This predates
the new early-storage overlay. The boot-owned kernel changes contain no USB
driver edit; USB commits inherited through the PCIe dependency were already in
the tested integration. The packed base and new overlay have identical
`setup_gadget`, `setup_network` and `start_netconsole` functions (checked offline).
The overlay does not rebind the gadget at storage probe or switch_root. Its new serial forwarder stops
before systemd. There is no evidence yet tying the ECM failure to ANS or fixing
it with this patch.

Netconsole shares the ECM data path and may fail with it. ACM is the independent
USB fallback; Wi-Fi SSH is available after the root system boots. To use Wi-Fi
for the same install/stage/check scripts, set `OMARCHY_HOST` to the iPad's actual
Wi-Fi address before running them (the SSH wrapper supports it). Note which
transport worked rather than classifying an ECM-only failure as a boot failure.

## Round 3 — retained PCIe state and rescue before host probe

**Test 2 result:** `c13f4742cf` entered Linux successfully, but port 0 timed out
in LTSSM `0x3` (`POLL_COMPLIANCE`), before NVMMU/ANS initialization. See the
[screen](../release/OMISSIONS.md). This moves the observed failure
from test 1's ANS enable to PCIe link training. No Round 3 device test was run
by this agent. The changes below supersede the earlier outgoing L2/power-off
sequence and the ANS-only early-USB gate.

### Diagnosis and change

The teardown and probe did not form a reset pair. `port_sleep()` sent
PME_Turn_Off (`port+0x8c = 0x31`), asserted PERST (logical 1, physical low),
then cleared RC refclk/power/enable. It did **not** stop LTSSM (`port+0x80`
bit 0). Probe set that same bit with OR, without clearing the PME request or
forcing an off/on port edge. Its "leave the reset default" refclk policy also
assumed an iBoot state that a kexec does not restore. Test 2's unchanged common
and PHY tunables confirm retained configuration; they do not identify which
physical signal stopped training. `0x3` means compliance, not L0 or L2.
The precise electrical cause remains unverified without the next device run.

The kernelcache confirms a writable LTSSM enable/disable bit in
`AppleS800xPCIePort_setLTSSMEnabled` (`fffffff005ee7a60`), the per-port power
sequence in `enablePortHardware` (`fffffff005ee73dc`) and its inverse in
`disablePortHardware` (`fffffff005ee72c8`). These names were saved in the shared
Ghidra project. The [existing findings](../pcie/kernelcache-notes.md#q3-port-bring-up-sequence-s8003)
record refclk gating bit 8 (its meaning inferred from the Apple caller),
NVMe-mode disable at RC `+0x24`, and the lane PHY programming. Round 3 uses
these known per-port controls; it does not invent an ANS PMGR/RTKit reset.

> Private graphics-dependent excerpt omitted from this source snapshot.

### Debugging a missing root

With `CONFIG_NVME_APPLE_ANS_EARLY_USB=y`, j71t now also defaults
`pcie_apple_h9p.probe=N`. The host returns deferred **before allocation/MMIO**,
so `/init` can configure ECM+ACM, getty/telnet rescue and ACM kmsg streaming
before any PCIe training. After `EARLY USB READY`, the helper opens both host
and ANS gates and writes `610000000.pcie` to platform `drivers_probe` in a
background worker, then reprobes deferred Apple storage BDFs. It never waits
for that worker in PID1. Missing ACM/unbound UDC leaves both gates closed.

The boot overlay defaults to a 60-second NVMe wait and **rescue**, instead of
another 600-second NBD fallback. `omarchy.nvme_wait` is capped at 120 seconds;
explicit `omarchy.fallback=nbd` still opts into NBD. On failure it prints the
gates, host binding, deferred devices if debugfs is available, and PCI/block
inventory; the ACM shell and log stream remain active. Other base initramfs
files and gadget setup are preserved. A CPU/fabric hard lock can still stop
USB: the gate gets diagnostics started earlier, not around a stopped CPU.
The ordinary cold-boot `g_multi -22` message is not changed by these patches.

### Build evidence

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

Logs: `build/boot/round3-{integrate-check,build,initramfs,policy-tests,ans-tests,fdt,payload}.log`.
Reproduce using the warm-reboot build commands and the payload arguments above,
then record the merge parents and verify the packaged entry:

```sh
scripts/build-kernel.sh --run integ \
  'git log --first-parent --format="%H%x09%s%x09%P" hoolock..HEAD > /work/build/boot/round3-integration-ancestry.txt'
python3 -B -I scripts/boot/verify-payload.py --tree integ \
  --ancestry build/boot/round3-integration-ancestry.txt \
  --manifest build/boot/round3-build.json
```

Kernel patches are exported as `patches/boot/kernel/0005-*` and `0006-*`.
Workspace script/test/doc changes are deliberately **uncommitted** for the
orchestrator. Only the two `a9/boot` commits were made directly; integration
merge commits were produced by `scripts/integrate.sh`.

Offline checks exercise the production restart/shutdown functions with mock
MMIO: cold and retained registers, powered/off entry, both ports without
cross-port reset, reset/clock timing, stale link controls, retry success and
exhaustion, L0 timeout, power timeout, missing capability, and ANS shutdown
fault paths. Shell fixtures verify both gates stay closed without diagnostics
and a blocked host reprobe leaves PID1 runnable. Packed-initramfs checks verify
the USB ordering, rescue defaults, single gzip stream and preserved base files.
These are software checks, not evidence of successful physical link recovery.

### Orchestrator-only device plan

> Private graphics-dependent excerpt omitted from this source snapshot.

   ```sh
   scripts/omarchy/netconsole.py build/boot/round3-netconsole.log
   scripts/boot.sh pongo
   scripts/boot.sh linux build/boot/m1n1-linux-boot.bin
   scripts/boot/install-device.sh
   TREE=integ scripts/boot/stage-kernel.sh --default \
     --cmdline "$(cat build/boot/integ-entry/cmdline)"
   scripts/boot/install-device.sh --check
   scripts/omarchy/ssh.sh 'uname -a; cat /proc/cmdline; cat /proc/sys/kernel/random/boot_id; cat /sys/devices/system/cpu/online; findmnt /; cat /sys/module/pcie_apple_h9p/parameters/probe /sys/module/nvme/parameters/apple_ans_probe; sudo dmesg | grep -Ei "EARLY USB|PCIe host|port [01]|NVMMU|ANS|timeout|error"'
   ```

   Require `EARLY USB READY` **before** host probe/training, port 0 and port 1
   `link up (LTSSM 0x11)`, NVMMU ready, NVMe root on the existing namespace-1
   window, both CPUs and usable SSH. A port-1 wait should advance or print its
   explicit 30-second exhaustion. Stop if the cold baseline regresses.

2. **Same-entry normal kexec, then repeat 3 times if it passes.** Record boot
   ID before each jump; retain the exact entry and command line:

   ```sh
   scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; sudo linux-a9-kexec load default; cat /sys/kernel/kexec_loaded'
   scripts/omarchy/ssh.sh 'sync; sudo systemctl kexec' || true
   # After USB/Wi-Fi returns:
   scripts/omarchy/ssh.sh 'uname -a; cat /proc/sys/kernel/random/boot_id; cat /sys/devices/system/cpu/online; findmnt /; sudo dmesg | grep -Ei "EARLY USB|PCIe host|port [01]|NVMMU|ANS|kexec|timeout|error"'
   ```

   Require changed boot ID, both ports in L0, NVMe root and SSH. Save inherited
   register dumps and any `retrying with port reset` line. Requiring the retry
   every time is evidence to investigate, even if boot succeeds. Outgoing
   logs, if captured before USB shutdown, should say `ports powered`.

3. **If network is down, use ACM; reselect its node after re-enumeration.**
   Run these on the Mac (Control-A, then `k`, then `y` exits screen):

   ```sh
   ls /dev/cu.usbmodem*
   # Substitute the Linux ACM node printed above:
   script -q build/boot/round3-acm.log screen /dev/cu.usbmodemNNNN 115200
   ```

   Press Return for the initramfs root shell. The background stream includes
   the kernel backlog; if it obscures the prompt, run `ps`, identify only the
   `cat /dev/kmsg` forwarder and kill that PID. Collect read-only diagnostics:

   ```sh
   cat /proc/uptime /proc/cmdline
   dmesg | tail -250
   cat /HL_init.log
   cat /sys/module/pcie_apple_h9p/parameters/probe
   cat /sys/module/nvme/parameters/apple_ans_probe
   ls -l /sys/bus/platform/devices/610000000.pcie/driver
   ls /sys/bus/pci/devices /sys/class/block
   cat /config/usb_gadget/g1/UDC
   ip link
   mount -t debugfs debugfs /sys/kernel/debug
   cat /sys/kernel/debug/devices_deferred
   cat /sys/kernel/debug/pm_genpd/pm_genpd_summary
   ```

   `EARLY USB READY` without `platform PCIe host probe returned` localizes the
   stall to the background host probe. A driver symlink also exists while a
   probe runs; use the probe completion/link/error messages to distinguish it
   from completed binding. Port 0 disabled localizes the missing root to
   training; L0/NVMMU ready plus ANS RDY timeout localizes it later. Port-1 GPIO progress/exhaustion distinguishes supplier deferral
   from a stopped kernel. If neither uptime nor ACM progresses, record the
   last framebuffer line and recover through DFU; do not poke MMIO.

4. **Bounded-failure check after successful warm storage.** This deliberately
   selects an absent root node, without changing the NVMe write window or
   default entry. Expect the host helper to time out waiting for SSH, then
   inspect ACM for `No NVMe root ... after 5s` and `RESCUE` with a working shell:

   ```sh
   WAIT_SECONDS=40 scripts/boot/kexec-host.sh \
     build/kernel-integ/Image.gz build/kernel-integ/dtbs/s8003-j71t.dtb \
     "$(cat build/boot/integ-entry/cmdline) omarchy.nvme=/dev/round3-missing omarchy.nvme_wait=5 omarchy.fallback=rescue"
   ```

   Keep this negative test last and recover with the cold payload. Its staging
   uses the existing namespace-1 root window; protected namespaces and factory
   storage are untouched.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Round 4 — PMGR adoption and deferred-probe visibility

### Device evidence and what it establishes

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

These are **completed** `really_probe()` calls. PMS itself returned, including
its binding/supplier-sync work. The old trace cannot identify the next
deferred device, a later sync callback, an interrupt storm or a fabric lock.
Round 4 fixes a source-level retained-state hazard and removes a diagnostic
barrier; it does not claim that the exact hardware cause is proven or that
warm reboot now passes. No device access was performed by this agent.

### PMS and the next probe candidates

`s800-0-3-pmgr.dtsi` names offset `0x80120` **`pms`**, physical
`0x20e080120`, with `apple,always-on`. The j71t ADT's `PMS` record is device
ID **53**, map **2**, index **4**, flags `0x23`; `ps-regs[2]` selects main
PMGR reg 0 at `0x80100`, so `0x80100 + 4*8 = 0x80120`. It is distinct from
`PMP` (`0x802e8`) and `PMS_SRAM` (`0x802f0`). Neither the merged j71t DT nor
the saved ADT identifies PMS as the USB or PCIe power domain. There are **no
DT power-domain consumers of `ps_pms`**. Do not reset or power-cycle PMS.

The next provider in DTS declaration order is `pcie_ref@80148`, followed by
the SIO descendants; that is not a guaranteed deferred-queue order. The
observed HSIC0 provider retry and still-deferred AOP GPIO make other genpd
providers/AOP suppliers, their dependent GPIO/I2C/serial consumers and
per-device `sync_state` callbacks candidates. PCIe can retry when suppliers
arrive, but its gate returns before host allocation/MMIO until `/init` opens
it. Port training cannot explain this pre-release stall through that path.

> Private graphics-dependent excerpt omitted from this source snapshot.

Reproduce the DT/ADT mapping offline:

```sh
python3 -B -I scripts/boot/pmgr-map.py > build/boot/round4-pmgr-map.json
```

The output includes all main-PMGR domains and their direct DT consumers,
including disabled consumers. It contains topology, not live register values
or the deferred queue. No pre-jump PMGR register capture exists in the supplied
evidence, so a numerical iBoot-versus-outgoing comparison remains a device
measurement. The new `raw before floor` messages capture both baselines before
this driver's min-state/adoption writes. Only those messages are raw snapshots;
`on begin`/`off held` use a zero placeholder before accessing the register.

### Source findings and fix

> Private graphics-dependent excerpt omitted from this source snapshot.

`sync_state_resume_initcall` had already returned in test 4, but a later
consumer bind can still invoke an individual supplier's sync callback.
`genpd_power_off_unused` is `late_initcall_sync`: PID1 cannot reach that global
pass until the synchronous deferred initcall returns. Earlier provider/runtime
work can nevertheless queue power-off work. PMS is protected by ALWAYS_ON;
the frame alone is not evidence that the global unused pass powered it off.

### Diagnostics and USB before an unrelated stuck worker

`driver_boot_trace=N` has a shared **4096-message maximum**, zero by default.
It prints deferred-device begin/end, probe begin/end, supplier/runtime begin/end,
driver callback begin/end and supplier sync lock/begin/end. These are INFO
messages, so starts are visible without relying on a return-only DEBUG log.
For an unbound device the driver field can be its bus name; callback messages
carry the bound driver. Use device name, stage and nearby messages together.
`apple_pmgr.trace=N` independently caps PMGR messages at **1024**, including
raw incoming state, ON/OFF starts, completed sets and held-off attempts.

The built payload also sets **`apple_boot.deferred_async=1`**. It takes effect
only with the early-USB config, j71t compatibility and the kexec chosen marker;
cold boot retains both original deferred-initcall flushes. On warm entry the
two flushes are skipped, leaving the existing deferred work queued while PID1
can start `/init`. The existing initramfs waits up to five seconds for a UDC,
configures ECM/ACM and starts rescue before releasing PCIe/ANS in a background
worker. No USB driver or gadget setup was changed in Round 4.

This is a diagnostic bypass, not a timeout/cancellation of the stuck probe.
It cannot bring up USB if the blocked worker has not supplied DWC2, holds a lock
needed by later initcalls/configfs, or stops a CPU/fabric. Later async-init
barriers can still wait for genuinely asynchronous probes. No probe is killed
and no generic lock timeout was invented. Keep the normal flush mode as the
first isolation check after the protective warm boot succeeds.

### Build and offline evidence

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

Logs are `build/boot/round4-{integrate-check,build,objects,pmgr-tests,pmgr-integ-tests,initramfs,policy-tests,ans-tests,check-build,fdt,payload,prepare,verify}.log`;
the final manifest records all nine parents, hashes and the full cmdline.
Rebuild with the nine-branch commands in the main runbook, then:

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

### Ordered orchestrator-only device plan

> Private graphics-dependent excerpt omitted from this source snapshot.

   ```sh
   scripts/omarchy/netconsole.py build/boot/round4-netconsole.log
   # Run the remaining commands in another terminal:
   scripts/boot.sh pongo
   scripts/boot.sh linux build/boot/m1n1-linux-boot.bin
   scripts/boot/install-device.sh
   TREE=integ scripts/boot/stage-kernel.sh integ-f53bf861ca --default \
     --cmdline "$(cat build/boot/integ-entry/cmdline)"
   scripts/boot/install-device.sh --check
   scripts/omarchy/ssh.sh 'sudo dmesg' > build/boot/round4-cold-dmesg.txt
   scripts/omarchy/ssh.sh 'uname -a; cat /proc/sys/kernel/random/boot_id; cat /sys/devices/system/cpu/online; findmnt /; cat /proc/cmdline'
   ```

> Private graphics-dependent excerpt omitted from this source snapshot.

2. **Same-entry warm boot with all protective settings.** Record boot ID and
   capture ACM/netconsole; a host disconnect alone is not success:

   ```sh
   scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; sudo linux-a9-kexec load default; cat /sys/kernel/kexec_loaded'
   scripts/omarchy/ssh.sh 'sync; sudo systemctl kexec' || true
   # Only after the new USB/Wi-Fi transport returns:
   scripts/omarchy/ssh.sh 'sudo dmesg' > build/boot/round4-warm-dmesg.txt
   scripts/omarchy/ssh.sh 'uname -a; cat /proc/sys/kernel/random/boot_id; cat /sys/devices/system/cpu/online; findmnt /; cat /proc/cmdline'
   python3 -B -I scripts/boot/pmgr-map.py \
     --cold-log build/boot/round4-cold-dmesg.txt \
     --warm-log build/boot/round4-warm-dmesg.txt \
     > build/boot/round4-pmgr-comparison.json
   ```

> Private graphics-dependent excerpt omitted from this source snapshot.

3. **If it stalls, identify the unmatched start before changing any setting.**
   Re-select the Mac's `/dev/cu.usbmodem*` and use the existing `script ...
   screen ... 115200` procedure. If ACM exists, collect `/proc/uptime`,
   `/proc/cmdline`, `dmesg`, `/HL_init.log` and `ps`; these establish that PID1
   ran and whether a worker or the whole system stopped. A deferred begin
   without callback begin includes supplier matching/device locks; callback
   begin without end localizes inside platform PM attachment or the driver;
   sync lock/begin without end localizes supplier synchronization. A PMGR ON
   begin without a completion/error can indicate a stuck MMIO/lock rather than
   an ordinary expired PS handshake. Timeout errors carry the final register.
   Trace budgets can exhaust, so absence of later trace alone is not proof.

   Use the Round 3 gate/PCI inventory diagnostics if the shell works. Run
   debugfs genpd summaries only as separate commands: a stuck genpd lock may
   block that reader too. Photograph the final framebuffer if there is no USB.
   Do not poke MMIO, reset PMS or reset the shared fabric. Recover via DFU.

4. **After step 2 passes, isolate the deferred-flush bypass.** Keep PMGR hold
   and the same Image/DTB/initramfs, changing exactly one token:

   ```sh
   args=$(sed 's/apple_boot.deferred_async=1/apple_boot.deferred_async=0/' build/boot/integ-entry/cmdline)
   WAIT_SECONDS=120 scripts/boot/kexec-host.sh \
     build/boot/m1n1-linux-boot.bin '' "$args"
   ```

   Require the same success criteria and save full dmesg. Success shows the
   hold/normalization can complete the normal synchronous deferred pass. A
   failure after the protective boot passed points to a remaining worker or
   dependency problem hidden by the bypass. Recover physically if needed;
   default remains the protective staged entry. Repeat successful normal
   synchronous boots before drawing a stability conclusion.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Round 5 — suspended-state panic recovery and phandle-safe updates

### Evidence and scope

> Private graphics-dependent excerpt omitted from this source snapshot.

The next owner-reported failure followed s2idle on `integ-f53bf861ca` with
`pcie_apple_h9p.suspend=2` and `dwc2.s2idle_keep_gadget=Y`. The button caused
console output immediately; phoenix ran, and stage2 boot ID `1b977173…` linked
both PCIe ports but never reached ANS RDY. Reported registers were CAP
`0x20f00103ff`, VS `0x10001`, CC/CSTS `0`, BFH `0xbadadd`. No host-side fault
was detected at 100 ms after enable. Good warm boots become ready in roughly
10 ms. The previous panic's text was not retained, so its cause remains unknown.
An unbind/rebind attempt lost ACM; do not repeat that as a recovery procedure.

**All Round 5 results below are offline. No device access was performed.**
The original ANS firmware stall is not proven fixed. The patch uses identified
host/endpoint controls and adds a reproducible suspended-state crash test;
there is still no identified independent ANS PMGR domain or A9 RTKit interface.
A shared PCIe/PHY/PMGR cycle or guessed coprocessor command is not justified by
the ADT/kernelcache evidence.

### Additional non-suspend failure (orchestrator report, 12:40–12:57)

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

The old panic sysctls were active in the outgoing boot. They are ordinary kernel
variables and **do not carry across kexec**. Tri's saved config has softlockup,
buddy-hardlockup and hung-task panic defaults off, and the loaded cmdline shown
in the ladder does not turn them on. A later automatic panic would require
userspace to reapply that policy or another explicit panic/fault; another crash
hop also requires the new boot to rearm its crash image. The proposed
hang->watchdog panic->default->hang chain remains possible, not observed.

Two concrete source-level candidates deserve isolation before blaming ANS:

> Private graphics-dependent excerpt omitted from this source snapshot.

The ANS/SART lifetime gap described below is a third candidate after a panic,
now addressed with incoming isolation and the phoenix host-only path. Existing
link/PMGR polls have finite software budgets; that does not bound an MMIO
transaction or a generic lock. No new hardware reset is justified solely by
the non-suspend report.

A fourth boot commit adds `driver_shutdown_trace=N` (0 by default, maximum
4096 messages, independent of the boot trace budget). It records begin/end
around the global probe wait, cpufreq suspension, per-device lock acquisition,
runtime barriers and shutdown callbacks. With `driver_boot_trace`, PMGR trace
and ramoops, the next recovered console can distinguish an outgoing probe/
lock hang from an incoming provider/link/controller hang. This instruments the
waits; it does not skip or time out synchronization around live DMA.

Device-tool load/dispatch messages also go to `/dev/kmsg`, load messages include
the source boot ID, and full initramfs prints its boot ID into kmsg before the
storage gate. Phoenix's prepared stage2 command line carries
`linux_a9.phoenix_from=SOURCE_BOOT_ID`; the normal warm loader strips this
one-shot provenance on the next requested load. A recovered boot's cmdline and
retained console can thus identify the crash source even after the journal is
unavailable. The capture kernel deliberately leaves the primary ramoops area
untouched: a hung capture kernel cannot supply its own persistent console there.
A second full-kernel panic can replace the single panic record; archive recovered
records before repeating tests. Pre-ramoops failures, physical resets and fabric
hangs remain observability limits. This cannot retroactively recover logs from
the already stranded f53/tri chain, which had no reserved logger.

### What suspend and panic actually leave behind

H9P's mode 2 pins endpoints and root ports in **D0**, keeps NVMe's port linked,
and sends Wi-Fi's GPIO-powered port through PME/L2/PERST/port-off. Mode 3 can
also sleep NVMe's port. Genpd supplier links keep host domains available.
Thus NVMe-in-D3 is not the expected mode-2 state, although a panic can interrupt
any of the restore/reset steps. NVMe's full suspend path sends SHN; resume
schedules its asynchronous controller reset. A panic can catch that shutdown
or reset, old queue/HMB addresses, retained NVMMU state, or Wi-Fi still in L2.
The reported CC=0/CSTS=0 after the next PERST does not identify the coprocessor's
internal state. No new host fault also does not prove the ANS firmware is healthy.

Panic bypasses `device_shutdown()`. More concretely, the old phoenix `/init`
never opened either early-probe gate. It could jump to stage2 without binding
H9P, so neither its incoming reset nor its outgoing shutdown ran in that hop.
The old `reset_devices` argument alone did not open those gates.

Round 5 changes:

- Incoming j71t port 0 logs inherited SART/TCB registers and disables the old
  SART aperture **under asserted PERST**, before allocating/programming new
  NVMMU tables. Ordinary port restart remains the Round 3 sequence.
- Phoenix forces the ANS endpoint gate closed and explicitly opens/reprobes
  the host. It waits at most 45 seconds for host initialization and a new
  capture-only `610000000.pcie/crash_quiesce` operation. Old capture kernels
  without the capability parameter retain their legacy behavior and print a
  warning; the suspended-state test requires the new capture Image.
- That operation handles an **unbound** Apple storage endpoint, checks link
  availability, enables memory access, writes CC=0 **without SHN or queues**,
  and polls RDY=0 at most 2,000 times with 1–1.1 ms sleeps. Link loss or an
  all-ones CSTS ends polling. It then blocks bus mastering and parks **port 0**
  under PERST before retiring SART. A controller timeout is logged but physical
  isolation still proceeds. It refuses a bound NVMe driver or busy late-port
  worker. It issues no NVMe admin command or block write.
- Wi-Fi's port remains linked until its driver's normal shutdown. Parking it
  early would put a live driver behind a dead link. The usual kexec host
  shutdown subsequently parks the remaining ports after their children.
- A stalled host probe/quiesce causes phoenix to halt after its bounded wait,
  without attempting stage2 while a probe might still run. A CPU stuck inside
  an MMIO transaction cannot be made safe by a software deadline.
- `pcie_apple_h9p.panic_resume_once=Y` is a root-only, default-off test knob.
  On j71t, an armed crash kernel permits one deliberate panic at the beginning
  of H9P `resume_noirq`, before link/NVMMU restoration and endpoint resume.
  The flag is consumed first. No armed capture kernel means a warning and no
  injected panic. Nothing enables this in production boot arguments.

This gives phoenix an actual reset/disable/isolation role instead of relying
entirely on the final kernel's initial probe. If ANS still fails after it,
collect the new inherited-register and controller-disable results. Do not infer
that repeating host resets is an ANS power cycle.

### Panic records across both hops

With built-in ramoops, early j71t boot chooses an unused, aligned **2 MiB**
region using memblock after firmware/kernel/initrd reservations and before
paging, CMA and crashkernel allocation. It adds `/reserved-memory/ramoops@ADDR`
with `reg`, `no-map`, 1 MiB panic record, 1 MiB console buffer and a matching
FDT memreserve entry. It publishes a new verified FDT and updates the late
reserved-memory descriptor capacity for the added static node. It never
zeros the record area. Allocation/publication failures roll back the new
reservation; other boards are unaffected.

An inherited ramoops node is reused, including when outside the capture
kernel's usable-memory range. A capture boot **never allocates a new region**.
The j71t capture kernel skips the ramoops probe so its console cannot overwrite
the primary kernel's retained console before stage2 reads it. Both the kernel
file loader and the userspace changed-DTB merge retain the node/reservation.
This can be established by the first warm boot into Round 5; no m1n1 change or
fixed physical-address guess is needed. Keep all subsequent target kernels
ramoops-enabled and the reservation intact. DFU/power-loss retention is not
claimed.

Cold payloads, warm loader cmdlines and new updater bundles include
`crash_kexec_post_notifiers`. Without it the standard panic path can jump to
kdump **before** its kmsg dump. This standard option runs panic notifiers and
the dump first; a broken panic notifier can therefore delay/prevent kdump.
The console buffer is an additional breadcrumb path, not a guarantee against
a CPU/fabric failure. See the kernel's [ramoops documentation](https://cdn.kernel.org/doc/html/latest/admin-guide/ramoops.html).

The boot initramfs copies existing `dmesg-ramoops-*` and `console-ramoops-*`
into `/run/linux-a9/pstore` and prints them to the console before releasing
PCIe/ANS. Rescue retries do not reprint a copied record. `/run` survives
switch_root. `linux-a9-pstore.service` then archives those copies under
`/var/lib/linux-a9/pstore/NEW_BOOT_ID/` on the normal root. Neither helper
unlinks pstore records. This also survives systemd-pstore consuming the original
sysfs files later. Phoenix itself never mounts or writes the NVMe root.

### Watchdog settings during s2idle bring-up

The supplied source does not support attributing the panic to one of the new
sysctls without its text:

- `kernel/watchdog.c` uses `running_clock()` for softlockup timestamps.
  `tick_freeze()`/`tick_unfreeze()` suspend/resume both sched_clock and
  timekeeping for s2idle. Ordinary elapsed sleep should not by itself become
  a softlockup. Long resume work or a clock/interrupt defect remains possible.
- The buddy hardlockup detector compares the other CPU's hrtimer heartbeat;
  it is not a wall-time sleep timer. Asymmetric CPU/interrupt restart can
  produce a report. It cannot detect a whole-SoC stall with both CPUs stopped.
- Hung-task checks are suppressed between PM_SUSPEND_PREPARE and
  PM_POST_SUSPEND; frozen tasks are excluded. Timeout accounting uses jiffies.
  This does not excuse a real D-state stall during an extended resume.
- RCU uses jiffies-based grace-period deadlines. Its PM notifier expedites
  grace periods, rather than globally suppressing reports throughout resume.
  A slow IRQ-disabled callback or heavy synchronous console output can stall
  progress. The normal sleep interval alone is not evidence of such a stall.

The installer and rootfs now retain `panic_on_oops=1`, the existing `panic=0`
and the armed crash kernel, but set `softlockup_panic`, `hardlockup_panic`,
`panic_on_rcu_stall`, and `hung_task_panic` to **0**. Detectors/reporting stay
available; hung-task timeout stays 180 seconds. After retained logs and repeated
short/long sleeps pass, re-enable one detector at a time. A reasonable diagnostic
trial uses watchdog_thresh=30 (soft threshold about 60 s), RCU timeout=60 and
hung-task timeout=600, rather than making every bring-up delay fatal. Those
are suggested trial values, not new shipped defaults or proof against false
positives. The [watchdog documentation](https://cdn.kernel.org/doc/html/latest/admin-guide/lockup-watchdogs.html)
explains the threshold relationship.

### DTB merge semantics and offline evidence

Tools version **2** normalizes phandles by **absolute node path** in old,
running and new trees before the three-way comparison. It preserves the new
DT's IDs and assigns unused IDs to runtime-only paths such as phram regions.
Definitions and references move together. Provider `#*-cells` control the
specifier walk; GPIO/clock arguments are not remapped just because their
integer equals an ID. It handles the actual pair's clock/reset/power/MSI/IOMMU,
GPIO, pinctrl, supply, CPU/cache and nvmem references, plus runtime memory-region
and iommu-addresses references. General interrupt-map layouts are deliberately
unsupported and refused. New binding-specific reference properties need a
parser before being used in runtime fixups; opaque vendor cells are not guessed.

The m1n1-removed `/chosen/framebuffer@0` now compares equal when only its
phandle references changed numbers. Its runtime replacement keeps its proper
power-domain target. AOP's added node remains. Genuine overlapping property
edits, changed pruned nodes, removed fixed-up nodes, duplicate/invalid handles,
missing providers and malformed/truncated supported specifiers still fail.
Reservations and stale initrd/crash/seeding cleanup retain the previous policy.

`test-fdt-merge.sh` now defaults to the **actual supplied pristine pair**:
`build/boot/integ-entry/dtb` and
`build/kernel-tri/dtbs/s8003-j71t.dtb` (tri `865849b0ad`, including AOP).
Seventy-one old node phandles differ. The running tree is an explicit synthetic
m1n1 fixture (framebuffer rename, PSCI/CPU, phram and reserved panic memory),
**not a live FDT capture**. Tests verify reference targets, preserve every new
static phandle, keep new nodes and validate serialization independently with
dtc. Additional cases cover runtime-ID collisions, nested references, arguments
equal to IDs, actual conflicts, pruning, dangling references, bad layouts and
reservation cleanup. Run an additional merge with a captured actual FDT before
the changed-DTB device jump.

Host-only commands (already run by this implementation, except the later
orchestrator integration):

```sh
scripts/boot/test-fdt-merge.sh
scripts/boot/test-recovery-offline.sh
scripts/boot/test-offline.sh
scripts/boot/build-initramfs.sh
scripts/boot/test-ans-offline.sh
scripts/boot/test-update-offline.sh
scripts/build-kernel.sh --tree boot
TREE=boot scripts/boot/check-build.sh
```

The recovery suite compiles actual early-reservation and late-reservation-scan
C against real libfdt with a mocked memblock, and actual phoenix controller,
sysfs-guard and one-shot injection C with mock MMIO. Shell fixtures cover
success, legacy capture compatibility, absent/busy quiesce, blocked host probe,
pstore copy/print-once/archive and no deletion. Existing ANS tests still compile
the production normal shutdown/restart, including PERST-before-SART ordering.
These checks cannot prove physical link/coprocessor reset or RAM retention.

Final Round 5 kernel commits on `a9/boot`:

- `767dad02da8fe771c91c6566dd973b65b5c1931b`: incoming SART retirement and
  capture-only ANS quiesce/port-0 isolation.
- `df0ad2dae531842630819db6264ded93935d7d89`: early ramoops reservation,
  late reserved-memory capacity and capture-kernel record preservation.
- `54a19d61fefc56fef92bdde0f0c3a42004d0061e`: armed one-shot resume panic test.
- `5fd640de0b4b181cbff10161026b85543611f2fd` (head): bounded outgoing
  shutdown/probe/lock trace after the non-suspend failure report.

Patch exports are `patches/boot/kernel/0009-*` through `0012-*`.
Workspace changes remain uncommitted. No branch besides `a9/boot` was committed
or moved, and nothing was pushed. A read-only `git merge-tree --write-tree a9/tri a9/boot` succeeded without conflicts; this is not an integration build.

The clean Docker `a9/boot` build produced Image.gz and 104 DTBs. Required config
and clean-source checks passed, as did **65 offline tests**: 14 boot policy,
8 ANS/USB, 14 DTB merge, 4 recovery and 25 updater tests. The optional real-bundle
updater case was skipped; the actual old/new DTB pair was tested separately.
The final build is `a9/boot 5fd640de0b`.
Checkpatch reports zero errors, warnings or checks for the four commits.

Evidence: `build/boot/round5-{build,check-build,checkpatch,merge-tree,initramfs,policy,ans,fdt,recovery,update}.log`
and `build/boot/round5-build.json`. Exact artifact SHA-256 values:

- Standalone `build/kernel-boot/Image.gz`:
  `bea73e9e0d69cdca1a648d6da3876aa5922dc667e04597ba1c95e236b58d366c`.
- Standalone `build/kernel-boot/dtbs/s8003-j71t.dtb`:
  `dbc899a95f92c26aaa629bbf03a6244101eca07bde77c245d0f0fe9ed4b8e36b`.
- Updated `build/boot/initramfs-omarchy.gz`:
  `01e2793dff648c265522a7b0a097625d77a9a0d4a5dcb6761b494702e32ab675`.
- Actual old pristine DTB:
  `c99b765bde0956ef845cd7ccef0f2abfe68b3e49b5f5deb5816abf2ddbb002ae`.
- Actual tri/AOP pristine DTB:
  `fcc22884a3ada4fd2309b946e2937c92b9900e5130d981a40bfa5b20f03af75c`.

### Ordered orchestrator-only device plan

1. **Preserve artifacts; build an integration.** Keep the exact passing Round 4
   entry/payload and logs. This agent commits only to `a9/boot`; its standalone
   `build/kernel-boot` is compile evidence, not a replacement for the current
   all-workstream system. Integrate all required workstreams, including AOP,
   into a separate generated tree (the orchestrator owns these merge commits):

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

2. **Install tools v2 and stage; do not promote yet.** From a healthy Linux
   boot, stop automatic idle suspend for this test window (repeat that after
   each reboot). Save the actual running FDT and pristine-entry provenance:

   ```sh
   scripts/boot/install-device.sh
   scripts/omarchy/ssh.sh 'systemctl --user stop hypridle.service; cat /usr/local/lib/linux-a9/VERSION; cat /proc/cmdline; cat /proc/sys/kernel/random/boot_id; linux-a9-kexec status'
   scripts/omarchy/ssh.sh 'cat /sys/firmware/fdt' >build/boot/round5-running-before.dtb
   good_args=$(scripts/omarchy/ssh.sh 'cat /proc/cmdline')
   TREE=bootr5 scripts/boot/stage-kernel.sh round5-trial --cmdline "$good_args initcall_debug driver_boot_trace=4096 driver_shutdown_trace=2048 apple_pmgr.trace=1024"
   # Use the actual current entry's dtb for --old (not a freshly rebuilt file).
   python3 -B -I scripts/boot/device/linux-a9-fdt-merge \
     --old build/boot/integ-entry/dtb --running build/boot/round5-running-before.dtb \
     --new build/kernel-bootr5/dtbs/s8003-j71t.dtb -o build/boot/round5-live-merged.dtb
   scripts/omarchy/ssh.sh 'sudo linux-a9-kexec load round5-trial; cat /sys/kernel/kexec_loaded; readlink /boot/linux-a9/default'
   ```

   Require version 2, a successful actual-FDT merge and load, and the unchanged
   old default. Check the merged framebuffer, PSCI, CPU methods, memory/HMB,
   calibration/MAC and phandle targets. Do not bypass a merge refusal. The
   `--old` example is valid only if the running entry is still the saved
   `integ-f53bf861ca`; substitute its exact staged pristine DTB otherwise.

> Private graphics-dependent excerpt omitted from this source snapshot.

4. **Promote only this verified artifact, arm matching phoenix, then SysRq-c.**
   Both the running entry and default must now contain the new kernel and boot
   initramfs. `set-default` re-arms recovery; check it explicitly:

   ```sh
   scripts/omarchy/ssh.sh 'sudo linux-a9-kexec set-default round5-trial; sudo linux-a9-kexec panic-load; cat /sys/kernel/kexec_crash_loaded; cat /sys/module/pcie_apple_h9p/parameters/crash_quiesce_supported; cat /sys/module/kernel/parameters/crash_kexec_post_notifiers; sync'
   scripts/omarchy/ssh.sh 'sudo taskset -c 0 sh -c "echo c > /proc/sysrq-trigger"' || true
   ```

   Allow up to 150 seconds for USB/SSH. Require primary panic -> capture,
   `capture kernel: preserving j71t records`, `phoenix ANS: ... clearing EN
   without SHN`, its bounded disable result, port-0 isolation and completed
   quiesce -> stage2. Require changed boot ID, NVMe root, both CPUs/links,
   `/run/linux-a9/pstore/dmesg-ramoops-*` containing the SysRq panic/stack, and
   the same files archived under `/var/lib/linux-a9/pstore/BOOT_ID/`. Save those
   files off-device before the next test. A missing dmesg record despite an
   old console record is a failed panic-dump gate, not a pass.

> Private graphics-dependent excerpt omitted from this source snapshot.

6. **One normal short suspend/resume, then the deliberate suspended-state
   panic exactly once.** First verify mode 2 and keep_gadget, sync, run one
   ordinary `systemctl suspend`, press the button, and require the same boot ID,
   both links, root and usable SSH. Collect any detector reports with panic
   switches still 0. Then recheck crash_loaded=1, sync and arm the one-shot:

   ```sh
   scripts/omarchy/ssh.sh 'cat /sys/module/pcie_apple_h9p/parameters/suspend /sys/module/dwc2/parameters/s2idle_keep_gadget; cat /sys/kernel/kexec_crash_loaded; sync; sudo sh -c "echo Y > /sys/module/pcie_apple_h9p/parameters/panic_resume_once"; sudo systemctl suspend' || true
   # Owner presses the button once; do not send another suspend/panic.
   ```

   The panic should occur **inside H9P resume_noirq**, before PCIe restoration.
   Require the exact retained panic reason `j71t: deliberate panic before
   PCIe resume`, successful phoenix quiesce and a healthy full default boot.
   Save the full panic/console records plus the inherited SART/TCB, CC/CSTS
   disable result, new ANS ready time and boot IDs. Require the test knob to be
   N in the new kernel. This is the key suspended-state recovery gate. If
   stage2 again stays at CSTS=0, preserve logs through ACM and stop; the ANS
   firmware needs further investigation, not an invented PMGR write.

7. **Long sleeps and updater workflow only after those gates pass.** Run normal
   mode-2 sleep for over 180 seconds, then a longer idle interval, with detector
   panics still off. Compare pre/post boot IDs and retained logs. Re-enable one
   detector at a time only after clean repeats, then run the existing
   [updater trial/promotion/rollback plan](kernel-updates.md#orchestrator-device-test-plan)
   with an added-node DTB and tools v2. Restore the intended idle policy after
   testing; do not leave the one-shot fault injection enabled.

For each jump, a disconnected SSH connection is not success. Report boot IDs,
exact Image/DTB/initramfs hashes, elapsed recovery, root/CPU/link checks, and the
retained panic files. No device test or protected-storage write is authorized
for implementation agents by this plan; only the orchestrator executes it.

## Round 5b — fix the pre-paging FDT allocation

### Failure and confirmed cause

> Private graphics-dependent excerpt omitted from this source snapshot.

**The offending branch is `a9/boot`, commit `df0ad2dae5`.** The new
`apple_j71t_reserve_pstore()` runs inside `arm64_memblock_init()`, before
`paging_init()` establishes the linear map. Its physical 2 MiB reservation is
valid at that point, but `memblock_alloc()` for the FDT copy is not: the latter
converts its allocation with `phys_to_virt()` and immediately clears it with
`memset()`. The resulting write faults on the absent linear mapping. This
happens before `console_init()` and framebuffer registration, explaining both
the black screen and failure to reach USB initialization.

This was reproduced **with the unchanged failing Image**, using QEMU TCG in
EL1, j71t-compatible root, RAM `0x800000000..0x87bbe4000` and the no-map HMB at
`0x87ee00000`. It faults at:

```text
ELR 0xffff800080debbec = __pi_memset_generic+0x16c (dc zva, x8)
FAR 0xffff00007bbe1080 = the unmapped FDT-copy linear address
ESR 0x96000044        = write translation fault, level 0
LR  0xffff800081430e24 = memblock_alloc_try_nid+0xa0
```

Changing only the fixture root compatible to bypass the j71t helper makes
that same Image reach `console_init()`. The fixed Image also reaches it with
the j71t helper enabled. The old/fixed config and pristine j71t DTB are byte
identical. QEMU supplies virt's GIC/timer, not Apple's hardware; it does not
execute m1n1 or establish a successful device cold/warm boot.

Other suspects were checked:

- The saved failed payload extracts to the exact built `bootr5` Image, j71t
  DTB and initramfs. This is not an incorrect payload composition.
- Its PSCI m1n1 prefix has SHA-256
  `27414194ba1c0294674119a78ced8d77b7b851eb836befae9b70584c85b56da3`,
  identical to the passing Round 4 manifest. Neither m1n1 nor PSCI was changed.
- AOP's added node and its phandle renumbering pass the real old/new DTB merge
  tests. Its `builtin_platform_driver` probe runs after console initialization;
  the early pstore path does not interpret its observer references. No AOP
  source or DT change is needed for this fault.
- `crash_kexec_post_notifiers` selects the panic/kmsg-dump ordering. It neither
  allocates memory nor executes a notifier on a normal cold entry. Both the
  faulting and passing QEMU cases include it. It remains in the payload.
- The other Round 5 boot changes run in PCIe probe, resume or shutdown, after
  this failure point. This correction does not establish that the earlier
  intermittent warm/suspend ANS failures are fixed.

### Uncommitted fix and checks

`a9/boot` stays at **`5fd640de0b4b181cbff10161026b85543611f2fd`** with three
modified files: `arch/arm64/mm/init.c`, `arch/arm64/kernel/setup.c`, and
`arch/arm64/include/asm/mmu.h`. The exported plain `git diff` is
[`0013-arm64-fix-pre-paging-j71t-pstore.patch`](../../patches/boot/kernel/0013-arm64-fix-pre-paging-j71t-pstore.patch);
apply it with **`git apply`**, not `git am`. No workspace or kernel commit was
made and nothing was pushed. Other workstreams and `bootr5` remain unchanged.

The early helper now allocates both reservations **physically**, edits the
FDT through `early_memremap()`, then releases that temporary mapping. It marks
the record region no-map before paging and rolls back failed preparations
before paging too. Immediately after `paging_init()`, a new helper publishes
the permanent FDT linear alias with the saved physical address, before
`unflatten_device_tree()`. Thus no temporary/fixmap pointer is passed through
`__pa()` or retained by OF properties. Inherited-region, capture-kernel and
record-preservation behavior is retained.

The identical patch was applied, uncommitted, to **`a9/bootr5b` at
`359fe60627c88163f793487ba8bb6b9bea823b6e`**, retaining all eleven original
integration parents and the exact original config. It built Image.gz and
105 DTBs. There is no new integration commit/hash: provenance is the original
commit **plus the patch SHA-256** below. The source patch in both worktrees
compares byte for byte with the export.

Two checks now catch the regression:

- [`check-early-pstore.py`](../../scripts/boot/check-early-pstore.py) enforces
  the early allocation/publication contract and actual `setup_arch()` call
  ordering. `build-kernel.sh` invokes it before compilation whenever this
  helper is present. Running the builder on unchanged `bootr5` now refuses
  the original defect before touching its build artifacts.
- The real-libfdt production-C recovery fixture now uses `PROT_NONE` memory,
  explicit temporary mappings and a paging phase. It rejects the original
  pre-paging virtual allocator instead of making it work with `calloc()`.
  It verifies temporary-map release, permanent publication with the physical
  address, inheritance/capture guards, rollback and the late reservation scan.

[`test-early-boot-qemu.py`](../../scripts/boot/test-early-boot-qemu.py) supplies
the independent binary check described above. It uses the unmodified Image
and its matching `llvm-nm` symbols, with hardware breakpoints at the EL1 sync
vector and `console_init()`. At the latter it reads the live published FDT:
the fixed run has a 2 MiB aligned ramoops region at `0x87b800000`, `no-map`,
and the matching FDT memory reservation; the FDT physical address is
`0x87bbe1080`. These are **fixture addresses**, not guesses for a device run.
Its QEMU PID is always terminated explicitly; no existing QEMU/NBD process
is touched. QEMU reserves 33 GiB of sparse virtual address space to expose
the high physical range, without allocating 33 GiB of resident RAM.

**66 offline tests passed**, plus one optional real-bundle case skipped:
14 boot policy, 8 ANS/USB, 14 real-pair DTB merge, 5 recovery and 25 updater.
The recovery suite passed against both boot and integration sources. The old
source fails both new phase checks as expected. The three QEMU runs reproduce
the original fault, pass the compatibility control, and pass the fixed Image.
Checkpatch has zero errors/warnings and one `AVOID_EXTERNS` style check for a
prototype matching its adjacent header declarations.

`check-build.sh` passes its required-config checks but intentionally fails its
clean-source check: **the requested uncommitted patch is present**. Its saved
`DIRTY` lists exactly the three kernel files above; this check was not weakened
or bypassed. Evidence and complete hashes are in
`build/boot/round5b/build.json` and the logs beside it, including
`{build,objects,recovery,recovery-integration,recovery-before,build-guard-before,check-build,policy,ans,fdt,update,checkpatch,qemu-before,qemu-control,qemu-after,payload}.log`.

### Exact offline rebuild and payload commands

Run from the workspace root. The current `boot` and `bootr5b` worktrees already
contain this patch; these commands also prepare a missing `bootr5b` from the
**original pinned integration**, without merging newer workstream heads or
making commits. An unexpected source diff is refused.

```sh
scripts/build-kernel.sh --run boot '
set -eu
patch=/work/patches/boot/kernel/0013-arm64-fix-pre-paging-j71t-pstore.patch
test "$(git rev-parse HEAD)" = 5fd640de0b4b181cbff10161026b85543611f2fd
test -z "$(git diff --cached --name-only)"
if git diff --quiet; then git apply "$patch"; fi
git diff --binary | cmp - "$patch"
if [ ! -d /src/wt/bootr5b ]; then
    git worktree add -b a9/bootr5b /src/wt/bootr5b 359fe60627c88163f793487ba8bb6b9bea823b6e
fi
cd /src/wt/bootr5b
test "$(git rev-parse HEAD)" = 359fe60627c88163f793487ba8bb6b9bea823b6e
test -z "$(git diff --cached --name-only)"
if git diff --quiet; then git apply "$patch"; fi
git diff --binary | cmp - "$patch"
cp /work/build/kernel-bootr5/config .config
'
scripts/build-kernel.sh --tree bootr5b
scripts/boot/test-recovery-offline.sh --source /src/wt/bootr5b
scripts/boot/test-fdt-merge.sh build/boot/integ-entry/dtb \
  build/kernel-bootr5b/dtbs/s8003-j71t.dtb
scripts/build-kernel.sh --run bootr5b \
  'llvm-nm -n vmlinux > /work/build/boot/round5b/bootr5b-symbols.txt'
python3 -B -I scripts/boot/test-early-boot-qemu.py \
  --image build/kernel-bootr5b/Image.gz \
  --symbols build/boot/round5b/bootr5b-symbols.txt \
  --log build/boot/round5b/qemu-after-exceptions.log
```

The known failed payload is preserved as
`build/boot/m1n1-linux-boot-r5-359fe60627.bin`. Its original initramfs is saved
byte for byte at `build/boot/round5b/initramfs-r5.gz`. Reuse that initramfs and
the existing PSCI m1n1 for this isolation test; no initramfs/m1n1 rebuild is
needed. The exact replacement payload command is:

> Private graphics-dependent excerpt omitted from this source snapshot.


The extracted replacement Image matches the build; its DTB, config, initramfs,
PSCI m1n1 and all arguments except `linux_a9.entry` match the failed payload.
`crashkernel=256M`, `crash_kexec_post_notifiers`, `psci=1` and the write window
`1:3146240-7812095` are intact. SHA-256 values for this completed build:

| Artifact | SHA-256 |
| --- | --- |
| Uncommitted kernel patch | `a48090753f38e114dae5c351a4f6c999c40704949403f2635c1498f2138cf0c9` |
| `kernel-bootr5b/Image.gz` | `00657f51eb41b9e7b9878dbc959db66c25d6e61b47abf0409ce8d5e09a421a3c` |
| j71t DTB (unchanged) | `fcc22884a3ada4fd2309b946e2937c92b9900e5130d981a40bfa5b20f03af75c` |
| Original Round 5 initramfs (unchanged) | `c65e8350f0ca6feb8ba9460029db5c5ebd17787f44a47e5134f2c8ff099fcd9b` |
| Failed Round 5 payload, preserved | `c4d53791cc092f53bd6032105b3356f3d12e26d53c8fbd197e8b5ffded4668e2` |
| Fixed Round 5b payload | `fefebe570da423918df211f6bc48759704bdcd4f3ce4007c0e754cb5670b88c1` |

A later rebuild may change Image/payload hashes through kernel build metadata;
the unchanged base commits and exact patch identify the tested source.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Round 6 — SPI handoff, retained GPIO IRQs and bounded boot evidence

### New evidence and localization

> Private graphics-dependent excerpt omitted from this source snapshot.

**The final controller is SPI1/audio, not N1 touch.** The actual tri DT maps
`20a084000.spi` / AIC 189 / `ps_spi1` to `audio-codec@0` (CS42L81, GPIO CS16).
N1 is on **SPI3 at `20a08c000`**, AIC 191 / `ps_spi3`, GPIO CS102, reset75,
LDO73 and ATN65. Controller numbering such as `spi1.0` is dynamically allocated;
the MMIO address and child compatible are the useful identifiers. The old
banner occurs immediately **before `devm_spi_register_controller()`**. That
function allocates/configures chip selects and enumerates children, whose
probes can run synchronously before registration returns. CS42L81's probe
first enables MCA MCLK and then reads its ID over SPI. A missing N1 calibration
line alone therefore does not locate the failure in N1.

Ranked source candidates, with their limits:

> Private graphics-dependent excerpt omitted from this source snapshot.

### Why the unused-clock and genpd messages are expected

> Private graphics-dependent excerpt omitted from this source snapshot.

The SPI input clocks in this DT are fixed clocks. TCLK has prepare/unprepare
operations but no `is_prepared`; MCA MCLK also has no hardware `is_prepared` or
`is_enabled` callback. The common clock core falls back to software reference
counts for these queries: an unused zero-count TCLK/MCLK does not acquire an
extra hardware disable from this pass. An enabled consumer has a nonzero count
and is skipped. TCLK's explicit prepare/unprepare polls are bounded at 20 ms.
Thus these two printed headers do **not** establish that SPI/N1 or the USB PHY
was powered off. Round 6 retains the Round 4 hold and changes no global clock
policy, PMGR floors or USB PHY sequencing. Do not blanket-enable every domain
or reset PMS on this evidence. The existing production PMGR tests exercise the
held-off path with zero writes, including idle-floor interaction.

### Changes and diagnostic contract

- **Incoming warm j71t GPIO baseline:** before registering the GPIO chip and
  enabling chained parent IRQs, mask inherited GPIO input IRQ modes and clear
  the valid bank/group latch bits. Preserve output modes, muxes, data, pulls,
  grouping and peripheral pins. Consumers then configure their own IRQs.
  Cold boots and other boards retain their old initialization. An unmapped
  warm IRQ is masked and acknowledged individually, with a rate-limited
  `masked orphan warm GPIO IRQ group=... pin=...` report. Both AP and any
  successfully probed AOP GPIO instances use this warm-entry rule.
- **SPI outgoing shutdown:** stop/drain the SPI core queue, obtain runtime
  power, deassert core-owned GPIO selects, then stop CTRL, select the known
  non-DMA/non-IRQ CFG baseline, zero RX/TX counts, clear latched status and
  read back CTRL to complete writes. Keep power pinned until the jump. Failed
  queue stop, runtime resume or CTRL stop panics on the kexec path rather than
  knowingly jumping with an unsuccessful handoff. The core's queue poll is
  500 sleeps of 10–11 ms; generic lock acquisition and a bus transaction can
  still stall, which is why the separate deadline can be armed before shutdown.
- **SPI incoming normalization:** apply the same known-register idle sequence
  under runtime power before registering children. Preserve the existing
  per-transfer CTRL/FIFO startup sequence; no guessed reset, SmartIO command,
  DMA register or PMGR cycle is added. Reject RX data exceeding the bytes
  actually submitted in the current transfer, instead of accepting stale
  FIFO data as a new response. Success/error exits flush the stopped CTRL.
- **Probe breadcrumbs:** SPI logs idle begin/inherited CTRL/CFG/status/end,
  register/children begin/end, the first eight PIO begin/end pairs per
  controller, and errors. Codec logs probe entry, MCLK enable begin/end, ID
  read begin/end and MCLK disable begin/end. N1 logs resource/firmware,
  power/reset, HBPP, upload, execute, discovery and shutdown milestones without
  printing calibration or firmware bytes. Its existing teardown remains intact;
  power-on now guarantees an additional 5–6 ms with reset asserted and LDO low.
- **Software boot/handoff deadline:** build
  `CONFIG_APPLE_J71T_BOOT_WATCHDOG=y` and opt in with
  **`apple_boot_watchdog.timeout=120`**. A timer is armed at `subsys_initcall`,
  before ordinary device/deferred probes; expiry calls `panic()` with
  `j71t boot deadline expired ...`. Default is 0/off; nonzero values are limited
  to 30–600 seconds. Other boards and capture kernels cannot arm it. The
  root-only `/sys/module/apple_boot_watchdog/parameters/timeout` accepts 0 to
  synchronously disarm, or 30–600 to explicitly rearm for a shutdown/recovery
  test. The boot initramfs disarms **only after a usable root has been mounted
  and `boot_root()` has checked its transition directories**. USB setup or a
  rescue shell does not disarm it. Use the rebuilt Round 6 initramfs whenever
  this argument is supplied; the saved Round 5b initramfs lacks this disarm.

This deadline catches a sleeping probe/lock wait even when the scheduler is
otherwise healthy, which the softlockup detector alone would not detect.
It needs a live CPU processing timer interrupts. For this no-suspend diagnostic
window, `softlockup_panic=1 nmi_watchdog=panic watchdog_thresh=30` additionally
uses the existing softlockup/buddy detectors for CPU starvation or one CPU's
lost timer heartbeat. The boot config fragment now explicitly requires those
detectors. Do not infer whole-SoC/NMI coverage on A9: both CPUs/fabric stopped,
a dead timer, inaccessible pstore or a broken panic path can defeat recording.
Runtime detector sysctls remain the conservative Round 5 policy after userspace
applies it; the separate deadline is unaffected by those sysctls.

Ramoops/panic dump ordering remains Round 5/5b. **A newly entered kernel does
not inherit an armed crash image from the outgoing kernel.** An incoming
pre-root deadline panic may therefore retain text/display a stack and halt
with `panic=0`; automatic phoenix recovery requires that kernel's userspace to
have armed its own capture image. Do not promise pstore survival through DFU
or a physical reset. An outgoing deadline test on a healthy, crash-armed
kernel can exercise the complete panic -> capture -> stage2 record path.

### Source and offline evidence

The starting head was `9daf65830cdb40ed592129c1eb5230d35453c212`, which already
committed the Round 5b fix; this supersedes the older uncommitted-status wording
above. Kernel commits were made **only on `a9/boot`**. Workspace changes remain
uncommitted and no device, SSH, USB or protected-storage access was performed.

> Private graphics-dependent excerpt omitted from this source snapshot.

New functional commits:

- `97eeb749f17d4d2f2a5574653fc61f20ffcd2599`: incoming GPIO IRQ baseline/containment.
- `661f13de4a905dab48b4e344a71216dc77c38490`: SPI handoff, FIFO validation, N1/codec milestones.
- `a1ce00667d48de8816dc4a7d11884cee4b445572`: software deadline and Kconfig.
- Final `a9/boot`: **`b48c325d1a1dc8c10f821a6b8dff9811cb1f6d40`**. The preceding power-dependency
  merge `afd9bc9896` preserves both DVFS/deadline config entries and places the new
  Makefile/Kconfig entries away from AOP's insertion points.

The final pin-mode guard `b48c325d1a` additionally preserves outputs/peripheral
modes when acknowledging an orphan latch; it masks only actual GPIO input IRQ
modes. Patch exports are `patches/boot/kernel/0014-*` through `0017-*`. The final
branch, including its dependency merge, is the integration source of truth.
`git merge-tree --write-tree` with either tri `865849b0ad` or the original
Round 5 integration `359fe60627` succeeds with the same merged source tree
`2afd700808389a26910298d1165da9c90c6b7ce2`. These checks use the literal
commit hashes, not moving worktree refs.

The standalone clean-source Docker build produces Image.gz and 104 DTBs.
The additional all-workstream check is a **detached, uncommitted** checkout of
`359fe60627c88163f793487ba8bb6b9bea823b6e` plus the exact merged-tree diff saved
as `build/boot/round6/integration.patch`. Its config is the saved Round 5b
integration config plus `CONFIG_APPLE_J71T_BOOT_WATCHDOG=y`. No integration
branch or commit is created. See `build/boot/round6/` for manifests and logs.

**72 offline unit tests passed**, with one optional real-bundle updater case
skipped: 14 boot policy, 8 ANS/USB, 14 DTB merge, 5 recovery, 25 updater and
6 new peripheral/deadline tests. Three further production-C fixtures verify
PMGR/deferred-initcall/trace behavior. The six new tests run actual driver C
under ASan/UBSan: GPIO mode preservation (including orphan latches on outputs
and peripheral pins), orphan containment, SPI shutdown ordering/failures,
PIO no-progress/stale-FIFO rejection and trace exhaustion, N1 lifecycle,
software-timer guards/disarm/rearm/expiry, and root-transition disarm. The
peripheral/recovery/PMGR fixtures also pass against the detached integration.
These are modeled MMIO/IRQ/timer tests, not Apple hardware emulation.

Both **final unmodified Images** reach `console_init()` in the existing high-RAM
QEMU fixture, with the published 2 MiB no-map ramoops node and FDT memreserve.
QEMU does not exercise SPI, GPIO, USB, m1n1 or real warm/panic recovery.
Standalone `check-build.sh` passes required config and clean source. The pinned
all-workstream build produces **105 DTBs**; its j71t DTB is byte-identical to
5b, and its sole config delta is the software deadline symbol. Its `DIRTY`
correctly reports the nine modified source files and new watchdog file, because
this validation checkout intentionally has no integration commit. The clean
source check was not bypassed. No Round 6 payload or device entry was installed.

Checkpatch on the four functional commits has **0 errors, 1 warning, 0 checks**.
The warning asks whether the new file needs MAINTAINERS coverage; it is already
covered by `F: drivers/soc/apple/*`. Existing dependency code was not restyled.

The repeatable offline integration builder is
[`build-round6-check.sh`](../../scripts/boot/build-round6-check.sh). It uses the
pinned boot/base commits and original config hash, refuses unexpected source
changes, and never creates/resets a branch or commits:

```sh
scripts/boot/build-round6-check.sh
scripts/boot/test-warm-peripherals.sh
scripts/boot/test-warm-peripherals.sh --source /src/wt/bootr6-check
scripts/boot/test-recovery-offline.sh --source /src/wt/bootr6-check
```

Hashes and provenance are in `build/boot/round6/build.json`. Final SHA-256 values:

- Standalone `build/kernel-boot/Image.gz`:
  `0b599b9d337bf66505fbed7b34ab98614b7b983aa4ad6fdd8a96c71d9d0c1635`.
- Standalone j71t DTB:
  `c99b765bde0956ef845cd7ccef0f2abfe68b3e49b5f5deb5816abf2ddbb002ae`.
- All-workstream `build/kernel-bootr6-check/Image.gz`:
  `9ac2a0496842fff918b1fdf46271d52c59e5eff11165e55e2e9a7bce587819af`.
- All-workstream j71t DTB (unchanged from 5b):
  `fcc22884a3ada4fd2309b946e2937c92b9900e5130d981a40bfa5b20f03af75c`.
- Round 6 `build/boot/initramfs-omarchy.gz`:
  `287da701d10cbb487127a6efaad43fd9ae485d715369f4ab7aa22803d6c96c03`.
- Detached source overlay `build/boot/round6/integration.patch`:
  `a042d28b4a0408401399e6d18d73995f85f108c3f252d9ab08f6d855835daa7e`.
- Preserved Round 5b cold payload (unchanged):
  `fefebe570da423918df211f6bc48759704bdcd4f3ce4007c0e754cb5670b88c1`.

Logs include `build-final.log`, `integration-build-final.log`, `objects-final.log`,
`peripherals{,-integration}.log`, `pmgr{,-integration}.log`, `recovery{,-integration}.log`,
`policy.log`, `ans.log`, `update.log`, `fdt-{real,integration}.log`, `qemu{,-integration}.log`,
`check-build.log`, `checkpatch.log`, `reproduce.log`, `config.diff`, the pinned
merge checks and before/after ref inventories. No device result is claimed.


### Ordered orchestrator-only device plan

> Private graphics-dependent excerpt omitted from this source snapshot.

2. **Prepare a clean pinned integration and a trial entry; keep the old default.**
   On the host, after this implementation handoff, the orchestrator can create
   the integration commits; the implementation agent has not done so:

   ```sh
   INTEGRATION_TREE=bootr6 scripts/integrate.sh --check \
     359fe60627c88163f793487ba8bb6b9bea823b6e a9/boot
   scripts/build-kernel.sh --run bootr6 '
   cp /work/build/kernel-bootr5b/config .config
   scripts/config -e APPLE_J71T_BOOT_WATCHDOG
   make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
   '
   scripts/build-kernel.sh --tree bootr6
   TREE=bootr6 scripts/boot/check-build.sh
   scripts/boot/build-initramfs.sh
   scripts/boot/test-fdt-merge.sh build/boot/round5b/entry/dtb \
     build/kernel-bootr6/dtbs/s8003-j71t.dtb
   ```

> Private graphics-dependent excerpt omitted from this source snapshot.

   ```sh
   scripts/boot/install-device.sh
   scripts/boot/upload-entry.sh bootr5b-359fe60627-pstorefix build/boot/round5b/entry
   ```

   If already present, verify its Image/DTB/initramfs against that saved entry;
   never rebuild a substitute or bypass an immutable-entry refusal. Save the
   actual running FDT after installing the tools. Stage `round6-trial` with
   the running warm contract, plus:

   ```text
   initcall_debug ignore_loglevel driver_boot_trace=4096
   driver_shutdown_trace=2048 apple_pmgr.trace=1024
   apple_boot_watchdog.timeout=120
   softlockup_panic=1 nmi_watchdog=panic watchdog_thresh=30
   ```

   ```sh
   good_args=$(scripts/omarchy/ssh.sh 'cat /proc/cmdline')
   round6_args="$good_args initcall_debug ignore_loglevel driver_boot_trace=4096 driver_shutdown_trace=2048 apple_pmgr.trace=1024 apple_boot_watchdog.timeout=120 softlockup_panic=1 nmi_watchdog=panic watchdog_thresh=30"
   TREE=bootr6 scripts/boot/stage-kernel.sh round6-trial --cmdline "$round6_args"
   scripts/omarchy/ssh.sh 'sudo linux-a9-kexec load round6-trial; cat /sys/kernel/kexec_loaded; readlink /boot/linux-a9/default'
   ```

   Staging must package the **new** boot initramfs. Use the existing load helper;
   require successful checksum/FDT validation, `kexec_loaded=1`, and the
   unchanged default. Keep `panic=0`, `crash_kexec_post_notifiers`, the NVMe
   write window and the Round 4 three-argument warm contract intact.

> Private graphics-dependent excerpt omitted from this source snapshot.

4. **Validate records/recovery before an induced deadline.** Promote only the
   artifact that passed those repeats, then arm its matching phoenix image.
   Run the Round 5 SysRq-c gate once, with `sync`, and require capture host-only
   quiesce, recovered NVMe root, both links/CPUs and an archived dmesg-ramoops
   panic stack. Save the records off-device. Then, on a healthy recovered boot
   with `kexec_crash_loaded=1`, no suspend and the same verified default, exercise
   the new timer once:

   ```sh
   # Device command, orchestrator only; expected deliberate panic after 30 s.
   sudo sh -c 'sync; echo 30 >/sys/module/apple_boot_watchdog/parameters/timeout'
   ```

   Do not feed/rearm it again. Require the exact `j71t boot deadline expired`
   panic reason in the newly archived record, successful phoenix/stage2,
   changed boot ID and deadline 0 after the new root transition. If the record
   or recovery is missing, stop; do not declare a hardware hang recoverable.

> Private graphics-dependent excerpt omitted from this source snapshot.

6. **Suspend remains a separate Round 5 gate.** After recovery and warm tests,
   confirm timeout 0 and restore the conservative detector panic sysctls before
   ordinary short/long s2idle or the armed one-shot resume panic test from
   Round 5. Do not carry a rearmed 120-second timer into a long sleep. Only
   restore normal idle policy after the intended tests have finished.

For each test retain the source/artifact hashes, full cmdline, old/new boot IDs,
root/link/CPU checks, outgoing/incoming phase records, `/proc/interrupts` when
available, pstore files and recovery duration. No implementation agent device
access or protected-storage write is authorized by this plan.

## Round 7 — enumerated ANS with stalled block I/O

### Evidence and limits of the diagnosis

> Private graphics-dependent excerpt omitted from this source snapshot.

This is later than the Round 2–5 link/CC.EN failures. A clean filesystem
unmount does not prove the host translation cache, firmware or interrupts are
healthy after the next reset. Conversely, partition enumeration normally
requires data-queue reads: this evidence does **not** establish that only admin
commands worked. No failed incoming dmesg, timed-out command/CQ snapshot or
ANS firmware trace was captured. The missing-systemd message is an initramfs
observation, not proof of filesystem corruption or of its precise failing I/O.
The fifth boot's physical root cause remains unconfirmed offline.

Source inspection found concrete recovery defects:

- `preinit()` retired/released FlatDMA slots **before** `CC=0`/`RDY=0`.
  During a controller reset those descriptors could still belong to old DMA.
  On a new host the software-used bitmap is empty, so this loop does not
  itself invalidate any inherited hardware cache entry. Host probe already
  issues NVMMU INIT; it was not missing from every warm boot.
- Failed invalidation cleared the host-used bit **before** the FIFO operation
  succeeded; the ANS completion path then released the same request tag anyway.
  Later I/O could reuse a translation whose invalidation failed. The old reset
  could no longer find it in the bitmap. Logging an error and continuing was
  insufficient; iOS instead treats the 20 ms FIFO failure as fatal.
- The timeout handler read endpoint CSTS **before** any ANS diagnostic or link
  guard. A stalled non-posted read can prevent the existing 30-second I/O timer
  from printing anything. The ordinary abort/re-arm path could then delay a
  failed mount across multiple I/O/admin timeout periods.

These are source-level failure mechanisms, not a claim that the fifth boot
hit a measured FIFO error. HMB/SART address readback, a stale completion,
firmware failing to execute a command, DART/NVMMU faults and interrupt delivery
remain distinguishable possibilities for the next captured failure.

**IRQ findings:** S3E is configured with `NVME_QUIRK_SINGLE_VECTOR` and
`NVME_QUIRK_SHARED_TAGS`. Admin Q0 and data Q1 both use vector 0; there is no
separate never-restored data-queue MSI vector in this configuration. Incoming
AIC initialization masks/clears sources and retargets them to CPU0; IRQ request
unmasks the allocated source. Port setup writes MSICTL and MSIVECBASE afresh.
An interrupt can still be lost later. A ready CQ entry recovered by the timeout
poll, combined with a flat IRQ count, is evidence for that case. No speculative
MSI/AIC register fix is added without that evidence.

The existing shared Ghidra analysis was checked offline:
`fffffff005eebf3c` (NVMMU active), `fffffff005eea0f0` (TCB base/init),
`fffffff005ee9fc0` (SART) and `fffffff005eebc9c` (FIFO-gated invalidate).
They confirm scratch → TCB base → INIT → SART programming and an invalidate
write without reading its command register back. The live ADT's mode **1**
supersedes older IPSW-only “mode 0” notes in
[the PCIe findings](../pcie/b1-hang.md). The
[Linux DMA guide](https://docs.kernel.org/core-api/dma-api-howto.html)
also requires ordering barriers for coherent descriptors; coherence alone does
not publish descriptor stores before a hardware command.

### Kernel and rescue changes

Kernel commit **`4e9a87e9efdbc03f104532dac072d4b28ac5ccdb`** on `a9/boot`:

1. Stop the controller and require `RDY=0` before rebuilding DMA state. Zero
   the driver-owned TCB/SGL/scratch allocations, drain the known invalidate
   FIFO with a 20 ms limit, disable SART before changing its address, publish
   WC memory stores, then issue the known NVMMU INIT with its existing bounded
   wait and restore the same SART aperture. Only a successful hardware init
   releases the software bitmaps/poison flag. Stop/init/FIFO failure prevents
   controller enable. A failed stop blocks later endpoint access.
2. Keep a failed invalidate's host/request bits allocated, poison new maps with
   `-EIO`, and schedule the existing asynchronous NVMe reset. Do not return a
   failed tag to the allocator. A synchronized stopped reset clears the poison.
   Normal successful invalidates retain the iOS-derived enqueue semantics;
   there is no new command-register readback or blind invalidate flood.
3. On every ANS request timeout, log the request tag/CID/QID/opcode, host
   PCIe/NVMMU/SART/DART state, invalidate FIFO/poison/used tags, and software
   SQ/CQ positions, next CQ status/CID/SQ head, queue DMA addresses and
   per-vector Linux IRQ counts **before** the first guarded CSTS read. The host
   dump no longer reads NVMMU `+0x4`, the invalidate command register.
4. Keep the existing missed-completion poll. For a still-outstanding j71t
   request in the live state, skip the admin-abort/re-arm cycle: allow one
   automatic controller reset per device lifetime; a second ordinary timeout
   disables the controller and fails pending I/O. Startup/reset/terminal-state
   handling remains the normal NVMe path. The quota is deliberately not reset
   by a successful soft reset. A completion found by polling retains the
   existing `completion polled` behavior and does not consume that quota.
5. Guard CSTS in enable/dead-controller checks as well as timeout. On j71t,
   CC.EN/RDY transitions have a default **5-second** software deadline via
   `nvme.apple_ans_rdy_timeout` (boot-time, clamped 1–30). A missed transition
   logs CC/CSTS plus host state and stops endpoint access. This does not shorten
   normal SHN handling or claim to time out a single wedged bus transaction.

> Private graphics-dependent excerpt omitted from this source snapshot.

Root-only `/sys/class/nvme/nvme0/apple_ans_diag` accepts:

```sh
echo host > /sys/class/nvme/nvme0/apple_ans_diag
# Explicit endpoint read, only after saving the host-only evidence:
echo endpoint > /sys/class/nvme/nvme0/apple_ans_diag
```

Both modes print the host/software queue snapshot to kmsg. `endpoint` then
uses the link/fault guard and reads CSTS, CC, AQA/ASQ/ACQ, HMB, INTMS and PCI
MSI configuration. It never reads write-only queue doorbells. A busy reset or
shutdown returns `EBUSY` after a host dump, rather than waiting for its mutex.
The default host mode performs no endpoint MMIO/config reads or block I/O;
host MMIO itself still requires a functioning SoC fabric.

The rebuilt boot initramfs packages **`/linux-a9-ans-state [--endpoint]`**.
It saves boot/cmdline/uptime, controller state, block statistics, timeout values
and `/proc/interrupts`, triggers the matching snapshot, then prints a dmesg
tail. The fallback path calls host mode automatically. A separate one-shot
worker also calls it after 30 seconds if the root transition has not happened,
including when PID1 is asleep inside `ext4_label()` or `mount()`. Successful
root transition cancels that worker and disarms the existing boot watchdog.
This helper is only in the initramfs; do not assume it persists after switch_root.

A software timeout needs a live CPU/timer. A stuck endpoint read can still
hold a CPU, and a stopped fabric can prevent both snapshots and pstore. The
Round 6 boot/handoff deadline remains the independent 120-second outer gate.
An incoming rescue kernel has no crash image unless its own userspace armed
one; deadline panic can retain evidence and halt, without automatic phoenix.

### Offline reproduction and artifact provenance

Only `a9/boot` was committed. Workspace scripts/tests/docs/patch export are
left uncommitted for the orchestrator. No device connection or push was made.
The integration is a **detached, uncommitted overlay** of the five Round 7
kernel files onto the exact tested Round 6 integration `d5822a7f71`; it creates
no integration branch or commit and does not pull newer workstream heads.
The tree's `COMMIT` names that base; `DIRTY`, `integration.patch` and the
Round 7 manifest describe the additional source. Do not identify it as an
unmodified `d5822a7f71` build.

```sh
scripts/build-kernel.sh --tree boot
TREE=boot scripts/boot/check-build.sh
scripts/boot/build-round7.sh
scripts/boot/build-initramfs.sh
scripts/boot/test-ans-io-offline.sh
scripts/boot/test-ans-io-offline.sh --source /src/wt/bootr7-check
scripts/boot/test-ans-offline.sh
scripts/boot/test-recovery-offline.sh
scripts/boot/test-warm-peripherals.sh
scripts/boot/test-offline.sh
scripts/boot/test-update-offline.sh
scripts/boot/test-fdt-merge.sh build/boot/round5b/entry/dtb \
  build/kernel-bootr7-check/dtbs/s8003-j71t.dtb
python3 -B -I scripts/boot/prepare-round7-entry.py
```

`test-ans-io-offline` compiles actual production functions under ASan/UBSan.
Fixtures inject stopped/stuck controller, failed hardware init, FIFO stall,
poisoned tag reuse, map direction, failed request retirement, link/host faults,
RDY deadline, CQ-polled completion, first/repeated timeout and controller-state
transitions; a shell fixture exercises both rescue modes without block devices.
It also checks that host snapshots never read the invalidate command register
or endpoint BAR/config. Existing tests cover shutdown/PERST/SART ordering,
early USB/gate/rescue behavior, packed initramfs preservation, pstore/phoenix,
GPIO/SPI/N1 handoff and immutable-entry/update/FDT policy. These establish
software behavior, not reliability on the iPad.

Final build checks, logs, entry and SHA-256 values are recorded in
`build/boot/round7/build.json` and `build/boot/round7/SHA256SUMS`.
Both full Docker builds passed (104 standalone and 105 integration DTBs),
as did **78 distinct offline tests**, with one optional real update-bundle
test skipped. The six new tests passed again against the detached integration;
the 14 FDT tests passed on both real pairs. Strict checkpatch reports zero
errors, warnings and checks. Ref inventories show only `a9/boot` changed.
The build/entry helper validates those saved test logs before emitting the
manifest; reproduction should redirect each suite to its corresponding
`build/boot/round7/*.log` as in the saved run.

SHA-256 values:

- Standalone `build/kernel-boot/Image.gz`:
  `6391ff02ac3df04bb83b5c9324820656ac923ea3f0a14f9a73df121fc41fbca4`.
- Standalone j71t DTB:
  `c99b765bde0956ef845cd7ccef0f2abfe68b3e49b5f5deb5816abf2ddbb002ae`.
- Pinned integration `build/kernel-bootr7-check/Image.gz` (the trial Image):
  `84cb02f2d6e70aa7390101701419b7c3ad41fb778fd37ce34f39ffca1570e0e1`.
- Pinned integration j71t DTB (unchanged from Round 6):
  `fcc22884a3ada4fd2309b946e2937c92b9900e5130d981a40bfa5b20f03af75c`.
- Preserved Round 7 `build/boot/round7/initramfs.gz`:
  `8855fe5f20a158aecf0692143a68ac02a9bf7eb2b6dc21a0026f0d3882ed03f8`.
- Exact detached overlay `build/boot/round7/integration.patch`:
  `ebe78b442e987f3035b300ad6a488fa6664332815c32ec45ff202ba8736cfa8b`.

The preserved local entry is `build/boot/round7/entry`, named
`round7-trial-4e9a87e9ef`. It was not installed, loaded or booted on the iPad.

### Ordered orchestrator-only device plan

> Private graphics-dependent excerpt omitted from this source snapshot.

2. **Stage the preserved Round 7 entry without changing the default.** After
   checking the manifest and hashes, install current boot tools and save the
   actual running FDT. `build/boot/round7/entry` contains the pinned integration
   Image/DTB, new initramfs and a proposed full command line derived from the
   saved successful Round 6 command line. Compare it with the actual running
   arguments before use; preserve the write window, `panic=0`, cold-restart
   guard, crash reservation and three-argument warm contract. It adds:

   ```text
   nvme.apple_ans_debug=1 nvme.apple_ans_rdy_timeout=5
   nvme_core.io_timeout=5 nvme_core.admin_timeout=10 nvme_core.max_retries=1
   ```

   Keep the Round 6 boot/shutdown/PMGR trace arguments and 120-second deadline.
   The per-request timeout values are a supervised diagnostic policy, not a
   performance recommendation. They are sampled when the controller/queues
   are created. Do not mistake the module parameter for every block queue's
   actual `queue/io_timeout` (milliseconds).

   ```sh
   # Host commands; implementation agent did not run them.
   scripts/boot/install-device.sh
   scripts/boot/upload-entry.sh round7-trial-4e9a87e9ef build/boot/round7/entry
   scripts/boot/install-device.sh --check
   scripts/omarchy/ssh.sh 'sudo linux-a9-kexec load round7-trial-4e9a87e9ef; cat /sys/kernel/kexec_loaded; readlink /boot/linux-a9/default'
   ```

   Require checksum/FDT validation, `kexec_loaded=1` and unchanged default.
   If this immutable entry name already exists with different content, choose
   a new name; never overwrite it or bypass the refusal.

> Private graphics-dependent excerpt omitted from this source snapshot.

   On every healthy boot record the new boot ID, expected kernel/entry, both
   CPUs and links, root source/rw mode, and the unchanged ramoops reservation.
   Require `controller stopped ... rebuilding DMA`,
   `NVMMU: stopped-controller cache/SART rebuilt`, usable root and deadline 0
   (read the root-only parameter with `sudo`). Save full dmesg, cmdline,
   `/proc/interrupts`, and a host snapshot before and after this read:

   ```sh
   # Device commands, orchestrator only, after reaching a healthy root.
   sudo sh -c 'echo 120 > /sys/module/apple_boot_watchdog/parameters/timeout'
   sudo sh -c 'echo host > /sys/class/nvme/nvme0/apple_ans_diag'
   sudo dd if=/dev/nvme0n1p2 of=/dev/null bs=1M count=64 iflag=direct status=progress
   sudo sh -c 'echo host > /sys/class/nvme/nvme0/apple_ans_diag'
   ```

   Arm the 120-second handoff deadline **before** a supervised read or the
   next shutdown, and explicitly disarm it after healthy read checks if the
   device will remain idle. Require zero new I/O timeout, completion-polled,
   reset, poison, DART/NVMMU error or deadline messages for a clean pass. A
   controller reset that makes root usable is a **recovered failure**, not a
   successful repeat. Stop at the first failure; 20 samples are a regression
   gate, not a statistical reliability claim.

4. **At the first rescue/stall, preserve host evidence before endpoint reads.**
   In the ACM/telnet initramfs shell run `/linux-a9-ans-state`; save the whole
   incoming dmesg and pstore records off-device while transport still works.
   The one-shot 30-second worker and I/O timer should already have printed
   their state. Identify records by boot ID; the big retained console dump at
   the screen top can describe the outgoing boot. Do not infer a new kernel's
   state from that dump. Only after the host evidence is saved, optionally
   run `/linux-a9-ans-state --endpoint` once. Avoid a second manual mount or
   unbind/rebind, which can block the remaining diagnostic shell.

   Interpret the first fault together with its CQ state:

   - `completion polled` with a ready CQ entry and flat IRQ count: completion
     DMA occurred; investigate MSI mask/address/data, AIC delivery and queue
     association using the saved endpoint and IRQ evidence.
   - No ready CQ entry, advancing SQ tail, healthy link and stable IRQ count:
     command execution/completion DMA is missing. Inspect SART/HMB addresses,
     DART fault/TTBR state, NVMMU errors and used tags; this alone does not
     prove whether the SQ fetch or data/completion DMA failed.
   - FIFO failure/poison or a DART/NVMMU fault: the first error precedes any
     recovery. Compare the pre-reset mapping state and the stopped rebuild
     result; do not blindly clear registers or release quarantined tags.
   - Snapshot present but no following CSTS line: endpoint access may itself
     have blocked. No snapshot/timer progress can indicate a CPU/fabric or
     earlier unrelated failure. Keep the framebuffer/last marker and recover
     using the established physical procedure.

   The first ordinary uncompleted timeout attempts reset; repeated ordinary
   timeout disables the disk. A failed RDY transition fails closed instead of
   retrying endpoint access forever. If the outer deadline panics, archive its
   retained text on the next software recovery; do not promise survival through
   DFU or claim the failed incoming kernel had armed phoenix.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Round 8 — checked DART activation and iOS-style ANS handoff

### Evidence and default

The [shutdown study](ds-nvme-b-ios-ans-restart.md),
[warm-boot forensics](ds-nvme-c-forensics.md),
[DART handler study](ds-nvme-a-dart-fault.md), and
[third Round 7 failure](2026-10-23-round7-warm3-rescue.md) supersede the early
interpretation of `err 0x500`. Bit 31 is clear and bits 6:0 are zero: that
record has **no valid DART exception**. Bit 10 records write direction; bit 8
is an attribute. FIFO `0xa01` is empty (bit 0 set). A stale DART translation
redirecting a successful CQ write to a previous kernel's backing page is the
leading hypothesis, **not a demonstrated hardware cause**. A discarded
attach-time flush result and lack of per-controller DART activation were real
source defects regardless of that hypothesis.

Kernel commits, only on `a9/boot`:

- `0324c6ba6501cb62d063081ae21cfc10a87bd9a0`: DART state before acknowledgement,
  ordered activation, checked flush/readback and error propagation.
- `5afd1e091d5a9dad905f776b0c7bd1faa81d0f80`: selectable handoff, complete
  port cycle, pre-publication health reads and one alternate-mode recovery.
- **`18eecae4429f02e4fba870b23a6da7e4f19fb84f`** (head): guard full-cycle port
  MMIO against partially powered inherited state.

Patches are in `patches/boot/round8/`. Workspace scripts/tests/docs remain
uncommitted for the orchestrator. No device connection or push was made.

`pcie_apple_h9p.ans_boot_mode=handoff|full-cycle|round7` is a boot-only parameter.
**The default is `handoff`**, a supervised trial default validated offline;
hardware promotion still requires the chains below. The checked DART flush is
mandatory in **both new modes**, so the handoff/full-cycle comparison isolates
the port/controller handoff question. `handoff` avoids the previous powered,
PERST-asserted half-state without adding an unnecessary port cycle. `round7`
retains the old PCIe/NVMe shutdown/bring-up and old timeout quota; the shared
DART attach hardening and pre-ack logging also apply to that control. It is
therefore not a byte-for-byte reproduction of the old Round 7 binary.

### Shutdown, activation and recovery contract

For the two new modes, normal kexec freezes/drains requests, synchronizes I/O,
requires an empty software mapping bitmap, and checks NVMMU FIFO empty with
the iOS **20 ms** bound **before** deleting queues and sending SHN. The generic
NVMe SHN wait is bounded; the ANS callback additionally requires successful
`CSTS.SHST=10b`. An undrained/dead controller, FIFO failure or SHST failure
panics instead of proceeding with an uncertified jump. It never clears CC.EN
on a successful new-mode shutdown. PCI BME and root routing are blocked.

- **handoff:** leave PERST, LTSSM, refclk, port power, DART, NVMMU and HMB
  intact. GPIO acquisition uses `GPIOD_ASIS`, rather than asserting reset
  before the fast-path test. Incoming checks powered/enabled/clock/ready,
  PERST deasserted, link up and L0, then only restores host IRQ/MSI routing.
  iOS `enableGated` at `fffffff005ed954c` tests its own `_state` (2/4/5),
  which cannot survive a kernel replacement. Our stricter hardware predicate
  is an explicit **inference**, not a claimed hardware test copied from iOS.
  An off or unhealthy inherited port takes full enable instead.
- **full-cycle:** after SHST, make the endpoint's DART streams inactive;
  record NVMMU inactive (Apple's false edge performs no NVMMU MMIO), assert
  PERST/stop LTSSM, clear RC **0x124**, wait **60 us**, clear **0x114**, then
  **0x100**. Incoming holds the port off at least 10 ms and follows the known
  power/ready/refclk/PERST/PHY/LTSSM sequence. The old driver's CTL/PWR/REFCLK
  macro names are misleading: the numeric offsets above define the order.
  Only port 0 is cycled; no shared PMGR or Wi-Fi DART reset is introduced.

On every new-mode controller start, PCI BME remains blocked while preinit
performs **CC=0 → RDY=0 → DART reinit → NVMMU scratch/TCB/INIT/SART**.
DART owns the new entry point and uses the domain's current page tables:

1. Print error/address/TCR/TTBRs **before any W1C/table replacement** in both
   `hw_reset` and `attach_config`/reinit. The first NVMe timeout still prints
   host state, queues and tag/CID/QID/opcode before endpoint access or recovery.
2. Disable the selected stream bytes (TCR=0), verify disabled, clear TTBRs;
   reflector → domain TTBRs (`BIT(31) | PA>>12`) → TCR → fetch → diag, matching
   `fffffff005fb5b58`'s actual order. Verify TCR **0x80 per owned stream**;
   enabling all four bytes to `0x80808080` would wrongly enable unowned streams.
3. Issue `sidmap<<8 | BIT(1)` and check the existing **100 us** busy poll.
   Verify all four TTBRs and TCR after flush. Any failure blocks translation,
   returns an error and prevents CC.EN.
4. `setup_translation` now returns flush errors to attach. Domain flush errors
   are returned where the IOMMU API permits it (`iotlb_sync_map`). Its void
   callbacks latch the error, disable the failed streams and block subsequent
   maps; they cannot return an errno to the IOMMU core. Only successful
   stopped-controller activation clears that latch.

After mappings are established, normal EnableController still programs
AQA/ASQ/ACQ → 0x1800 → 0x1810/0x1814/0x1818 → CC.EN → RDY=1.

Before `nvme_start_ctrl()` publishes namespaces, a temporary blk-mq queue uses
the **normal I/O tagset/SQ/CQ/IRQ/NVMMU path** to read namespace **1 only**:
identify its format, read **8 KiB at LBA 0** (MBR plus GPT header even with 4K
logical sectors), then **4 KiB at the existing root window's first LBA**.
There are no writes, vendor commands, namespace 2–7 operations or guard changes.
Each command has a **5-second timeout**, NOWAIT tag allocation and no retries.

On a health timeout, the handler prints tag/CID/QID/opcode and host/queue
state, flushes pending console output for at most one second (including active
netconsole), blocks PCI DMA and cancels requests **before freeing buffers**.
It avoids nested admin deletion or a CSTS read on that failure path. The reset
worker then consumes one lifetime recovery quota and uses the **other mode's**
bring-up, restores PCI configuration and re-runs checked activation and health
reads. The recovery quota is shared with later ordinary timeouts. A second
failure makes the disk DEAD and initramfs reaches rescue; a second alternate
mode recovery is not attempted. A recovery from full-cycle to handoff takes
the retained-link path when usable, or its explicit full-enable fallback if
the previous enable never established a usable link.

The health reads establish that these queue reads completed, not that all
future filesystem I/O is reliable. A single fabric-stalled MMIO transaction
can still prevent a CPU from reaching any software timeout. The independent
120-second handoff watchdog and preserved cold recovery remain necessary.
Panic/phoenix quiesce keeps its existing PERST/SART isolation; it does not
claim the graceful SHST handoff.

Every initramfs `rescue()` now calls the host-only `/linux-a9-ans-state` and
pstore reader directly on **`/dev/console`**, independent of USB/ACM/DTR or the
PID1 logger. The helper prints the **entire incoming NVMe/DART/timeout subset**
before its dmesg tail, preserving the first timeout after long pstore output.
Pstore records are copied/read, never unlinked. The original records remain
at `/sys/fs/pstore/`, with copies in `/run/linux-a9/pstore/`.

### Offline reproduction and artifacts

```sh
scripts/build-kernel.sh --tree boot
TREE=boot scripts/boot/check-build.sh
scripts/boot/build-round8.sh
scripts/boot/build-initramfs.sh
scripts/boot/test-ans-handoff-offline.sh
scripts/boot/test-ans-io-offline.sh
scripts/boot/test-ans-offline.sh
scripts/boot/test-recovery-offline.sh
scripts/boot/test-warm-peripherals.sh
scripts/boot/test-offline.sh
scripts/boot/test-update-offline.sh
scripts/boot/test-fdt-merge.sh build/boot/round5b/entry/dtb \
  build/kernel-bootr8-check/dtbs/s8003-j71t.dtb
scripts/boot/test-ans-handoff-offline.sh --source /src/wt/bootr8-check
python3 -B -I scripts/boot/prepare-round8-entries.py
```

Save suite output in `build/boot/round8/{handoff,ans-io,ans,recovery,peripherals,policy,update,fdt}.log`,
the standalone build in `build-final.log`, and integration build in
`integration-build.log`; the entry helper validates these logs.
The detached `bootr8-check` tree overlays **Round 7 plus Round 8** onto pinned
Round 6 integration `d5822a7f71cd`. No integration ref is changed. Device
entries use its unchanged tested config (including the write window) and j71t
DTB, not the standalone boot tree's read-only config. `COMMIT`, `SOURCE`,
`integration.patch`, `build.json` and `SHA256SUMS` identify the exact overlay.
All three entries have identical Image/DTB/initramfs; only entry identity and
the selected mode differ in their command lines. No existing cold payload is
overwritten.

Final offline result: both full builds passed (**104 standalone / 105
integration DTBs**), required standalone config/clean-source checks passed,
and all **88 distinct tests passed**, with one optional real update-bundle
test skipped. The 10 new production/fault/rescue tests and six queue-recovery
tests also passed against the detached integration. They include the actual
S5L8960X stream command encoding/busy poll and partially powered port cases.
Strict checkpatch on the complete source delta reports **0 errors, 0 warnings,
0 checks**. The exported series contains three commits. Other workstreams
advanced independently during the build; this task committed only `a9/boot`.

Verified trial artifacts (all three modes share these bytes):

- Image.gz: `9c28687ea67767f8f37527124cf8a65c42d827288004388455e9d216d65ae4e5`.
- j71t DTB: `fcc22884a3ada4fd2309b946e2937c92b9900e5130d981a40bfa5b20f03af75c`.
- Initramfs: `412b16a6722a8eec5fa86e8d30456c1f0161387491644a3f9e629547af41e6b7`.

The generated `build/boot/round8/SHA256SUMS` verified successfully. No hardware
result is claimed; the existing staged default has not been changed.

### Ordered orchestrator-only device plan

1. **Verify artifacts and start text capture.** From the workspace root:

   ```sh
   shasum -a 256 -c build/boot/round8/SHA256SUMS
   # Separate terminal; also keep ACM capture if available:
   scripts/omarchy/netconsole.py build/boot/round8/device-netconsole.log
   ```

> Private graphics-dependent excerpt omitted from this source snapshot.

   ```sh
   scripts/boot.sh pongo
   scripts/boot.sh linux build/boot/m1n1-linux-boot-r5b-359fe60627-pstorefix.bin
   ```

   Require working root/SSH, both CPUs and PCIe links, ramoops attachment and
   the existing `1:3146240-7812095` write window before proceeding.

2. **Install tools and stage all three entries without changing default.**
   Preserve the current entry/FDT as required by the existing boot contract:

   ```sh
   scripts/boot/install-device.sh
   scripts/boot/upload-entry.sh round8-handoff-18eecae442 build/boot/round8/entry-handoff
   scripts/boot/upload-entry.sh round8-full-cycle-18eecae442 build/boot/round8/entry-full-cycle
   scripts/boot/upload-entry.sh round8-round7-18eecae442 build/boot/round8/entry-round7
   scripts/boot/install-device.sh --check
   scripts/omarchy/ssh.sh 'readlink /boot/linux-a9/default; cat /proc/cmdline'
   ```

   Compare each complete command line with the actual running arguments before
   loading it. Keep the write window, `panic=0`, cold-restart guard, crash
   reservation and three-argument warm contract. Never overwrite an immutable
   entry with different contents. All trial entries use a 90-second root wait
   and the existing 120-second outer deadline.

3. **Transition into handoff, then run 20 matching warm jumps.**

   ```sh
   scripts/boot/warm-chain.sh round8-handoff-18eecae442 1 build/boot/round8/device/handoff-entry
   scripts/boot/warm-chain.sh round8-handoff-18eecae442 20 build/boot/round8/device/handoff-chain
   ```

   The transition tests incoming normalization; the 20-boot cohort tests
   Round 8 handoff on both sides. Require each boot's changed ID, expected
   entry/kernel, both CPUs/links, usable NVMe root, checked DART reinit,
   NVMMU rebuild, health-gate result **0**, and successful 64 MiB direct read.
   Matching handoff boots should print `handoff already-enabled L0; no port
   reset`; record any fallback separately. A timeout, flush/readback failure,
   alternate-mode recovery or completion-polled event fails a clean sample,
   even if root eventually becomes usable. The chain script stops on failure.

4. **Transition into full-cycle, then run its 20 matching jumps.**

   ```sh
   scripts/boot/warm-chain.sh round8-full-cycle-18eecae442 1 build/boot/round8/device/full-cycle-entry
   scripts/boot/warm-chain.sh round8-full-cycle-18eecae442 20 build/boot/round8/device/full-cycle-chain
   ```

   Exclude the handoff→full-cycle transition from the matching cohort. Require
   `full-cycle full port bring-up`, the same checked DART/NVMMU/health markers
   and the same root/read/CPU/link gates. Compare failure/recovery counts,
   first failing opcode/QID, DART flush results and queue backing state. Twenty
   samples per mode are a regression screen, not a reliability proof.

5. **Crossover back to the proposed default after both cohorts pass.**

   ```sh
   scripts/boot/warm-chain.sh round8-handoff-18eecae442 1 build/boot/round8/device/handoff-crossover
   scripts/boot/warm-chain.sh round8-handoff-18eecae442 5 build/boot/round8/device/handoff-confirm
   ```

   The first crossover may take full enable because the outgoing full-cycle
   kernel powered the port down. Subsequent matching handoff entries must
   take the retained path. Keep the staged default unchanged until results
   justify promotion. The `round8-round7-18eecae442` entry remains available
   as an explicit old-port-sequence control; it is not required to pass the
   new pre-publication health gate because that mode intentionally omits it.

6. **At the first failed sample, archive evidence before another jump.** If
   the one recovery succeeded and SSH returned, preserve the entire chain
   directory/dmesg and classify it as a recovered failure; switch to the other
   cohort only after that evidence is saved. If rescue is reached, the console
   automatically prints the first incoming NVMe timeout plus the host snapshot.
   Preserve `/sys/fs/pstore/*`, `/run/linux-a9/pstore/*`, incoming dmesg and the
   console before any endpoint read or DFU. `/linux-a9-ans-state` is host-only;
   use `--endpoint` at most once, after saving host evidence. Do not manually
   mount repeatedly or unbind/rebind the endpoint. Use the established cold
   recovery if transport/root does not return. Do not promise pstore survival
   through DFU. All namespace 2–7, syscfg, NVRAM and boot-firmware guards stay.

## Round 8b — fix the health gate before another device trial

> Private graphics-dependent excerpt omitted from this source snapshot.

### Exact cause and correction

Line 389 is `WARN_ON_ONCE(lim->max_hw_sectors < PAGE_SECTORS)`. The gate set
`max_hw_sectors = 16` (8 KiB), while A9's `PAGE_SIZE = 16384` requires
`PAGE_SECTORS = 32`. The field was nonzero, so the block layer did not supply
its default. This was a transfer-size/queue-capability confusion, not an
unset namespace logical-block size.

The gate now clones the admin queue limits already validated and updated
with the controller's Identify data in `nvme_init_ctrl_finish()`. It overrides
only logical and physical block size for the identified NSID 1 format. This
preserves the controller transfer/segment limits and conservative PRP
boundary/DMA-alignment rules. Reads remain 8 KiB at LBA 0 (MBR/GPT) and
4 KiB at the existing NSID 1 root-window start. Both reads must fit inside
the identified namespace. No writes or other namespace commands were added.

The gate also returns a separate `unhealthy` verdict. Allocation, request-tag,
mapping, queue-limit, unsupported-format and range errors are **inconclusive**.
Returned command errors are also inconclusive: a rejection alone does not
justify electrically restarting a responding controller. They print:

```text
Apple ANS: health gate partition/data NSID 1 result <error> (gate inconclusive)
Apple ANS: health gate inconclusive (<error>); continuing namespace discovery without recovery
```

The reset worker proceeds directly to LIVE and normal namespace discovery.
It does not disable the controller, spend the recovery quota, or cycle the
link on those errors. This is diagnostic fail-open with an explicit failed
sample, not a claimed health pass. A command timeout still takes the existing
bounded timeout path, snapshots before teardown, blocks DMA and cancels the
requests before their buffers are freed. Its `ANS_HEALTH_FAILED` bit is the
only gate verdict that authorizes the existing one recovery. It logs
`(controller timeout)`. Successful gates log `result 0 (passed)`.

The ANS DART predicate no longer infers a fault from an address change. It
requires `dart_err & BIT(31)`, matching the DART IRQ handler. `0x100` and
`0x500` with valid=0 are transaction records, regardless of address changes.
An IRQ-acked fault cannot be recovered from the address alone; retain the
actual DART IRQ/pre-ack logs for that evidence. The previous `0x500` message
was diagnostic, not itself the recovery trigger; the gate's `-EINVAL` sent
reset_work to recovery.

Kernel fix: `a9/boot` **`24ef187b3091aaeb17455fe2ba3bfcd9f935b603`**.
`patches/boot/round8b/` contains one incremental patch after `18eecae442`, plus
BASE/HEAD/SERIES and its checksum manifest. Only `a9/boot` was committed;
workspace scripts/tests/docs remain uncommitted. No device access or push.

### Round 8 provenance audit

The original `7.3.0-rc1-gd5822a7f71cd-dirty` version was expected. As documented
above, `bootr8-check` was deliberately a detached source overlay, not a new
integration commit. The audit compared all of the following offline:

- `git diff b48c325d1a..18eecae442`, the saved Round 8 `kernel.patch`, and
  the original `bootr8-check` worktree diff are byte-identical, SHA-256
  `041b10718b3a12e133ad1480f7daac4442bceae12daf0c2a5ed03829dda4b77a`.
- All three original entries contain the exact built Image.gz, SHA-256
  `9c28687ea67767f8f37527124cf8a65c42d827288004388455e9d216d65ae4e5`,
  and identify source head `18eecae442`. The image contains the Round 8 DART,
  handoff and health-gate markers and embeds the reported version string.

The base is therefore consistent with the intended build; it is not evidence
that only Round 7 ran. Audit results are saved in
`build/boot/round8b/{provenance.log,round8-provenance.json}`. Round 8b retains
that same pinned integration base/config/DTB, and adds an explicit
`-a9-boot-r8b-24ef187b30` release suffix without changing `.config`.
`build-kernel.sh` now exports the actual `RELEASE` and `SOURCE.patch`; the
builder and packager verify the version embedded in Image.gz and require the
source diff to equal the saved overlay exactly. No existing cold payload or
Round 8 artifact is overwritten.

### Offline reproduction

```sh
mkdir -p build/boot/round8b
python3 -B -I scripts/boot/check-round8-provenance.py
scripts/boot/test-ans-handoff-offline.sh > build/boot/round8b/handoff.log 2>&1
scripts/boot/test-ans-io-offline.sh > build/boot/round8b/ans-io.log 2>&1
scripts/boot/test-ans-offline.sh > build/boot/round8b/ans.log 2>&1
scripts/boot/test-recovery-offline.sh > build/boot/round8b/recovery.log 2>&1
scripts/boot/test-warm-peripherals.sh > build/boot/round8b/peripherals.log 2>&1
scripts/boot/test-offline.sh > build/boot/round8b/policy.log 2>&1
scripts/boot/test-update-offline.sh > build/boot/round8b/update.log 2>&1
scripts/build-kernel.sh --tree boot > build/boot/round8b/build-final.log 2>&1
TREE=boot scripts/boot/check-build.sh > build/boot/round8b/check-build.log 2>&1
scripts/boot/export-round8b.sh
scripts/boot/build-round8b.sh > build/boot/round8b/integration-build.log 2>&1
scripts/boot/test-fdt-merge.sh build/boot/round5b/entry/dtb \
  build/kernel-bootr8b-check/dtbs/s8003-j71t.dtb > build/boot/round8b/fdt.log 2>&1
scripts/boot/test-ans-handoff-offline.sh --source /src/wt/bootr8b-check \
  > build/boot/round8b/handoff-integration.log 2>&1
scripts/boot/test-ans-io-offline.sh --source /src/wt/bootr8b-check \
  > build/boot/round8b/ans-io-integration.log 2>&1
python3 -B -I scripts/boot/prepare-round8b-entries.py
shasum -a 256 -c build/boot/round8b/SHA256SUMS
```

The handoff suite compiles the tree's actual `struct queue_limits`,
`blk_set_default_limits()`, `blk_validate_limits()` and block-size validator.
Optional integrity/atomic/zoned subvalidators are mocked with assertions that
those unused features are absent. It reproduces the old 16-sector failure
on 16/64 KiB pages and the old success on 4 KiB pages, then verifies the fixed
production health function across those page sizes and 512/1024/2048/4096-byte
LBAs. It injects allocation/tag/map/queue/format/range/command errors and real
health-timeout classification. A separate fixture runs the actual reset
worker: local/command errors reach LIVE with zero disable/recovery calls;
timeouts receive only one recovery. Another exercises the actual DART
predicate over valid/invalid read/write records and changing addresses. All
C fixtures run with ASan/UBSan. Device reliability remains untested.

Final validation: **90 distinct tests passed**, one optional real update-bundle
test skipped; the 12 handoff and six I/O tests also passed against the exact
detached integration. Both full builds passed (**104 standalone / 105
integration DTBs**). Standalone required-config/clean-source checks passed;
strict checkpatch on the source delta and exported commit reports **0 errors,
0 warnings, 0 checks**. The export reverse-apply check passed, and the current
`a9/boot` delta equals the built integration overlay byte-for-byte.

The three trial entries share these verified bytes:

- Image.gz: `39046a54fbd28d937bf6a68e6f5c987d4cb2f507f800ca8ddcc5cc0d97554b0f`.
- j71t DTB: `fcc22884a3ada4fd2309b946e2937c92b9900e5130d981a40bfa5b20f03af75c`.
- Initramfs: `412b16a6722a8eec5fa86e8d30456c1f0161387491644a3f9e629547af41e6b7`.

The complete embedded release is
`7.3.0-rc1-a9-boot-r8b-24ef187b30-gd5822a7f71cd-dirty`.
`build/boot/round8b/{build.json,SHA256SUMS}` records the source overlay,
release, export, test logs and entry hashes; checksum verification passed.

### Ordered orchestrator-only device plan

1. Verify the new checksums and start capture in a separate terminal:

   ```sh
   shasum -a 256 -c build/boot/round8b/SHA256SUMS
   scripts/omarchy/netconsole.py build/boot/round8b/device-netconsole.log
   ```

   If the device is still in rescue, recover with the unchanged known cold
   gate before staging:

   ```sh
   scripts/boot.sh pongo
   scripts/boot.sh linux build/boot/m1n1-linux-boot-r5b-359fe60627-pstorefix.bin
   ```

   Require a usable root/SSH, both CPUs/links and the existing write window.

2. Stage all three immutable entries without changing the default:

   ```sh
   scripts/boot/install-device.sh
   scripts/boot/upload-entry.sh round8b-handoff-24ef187b30 build/boot/round8b/entry-handoff
   scripts/boot/upload-entry.sh round8b-full-cycle-24ef187b30 build/boot/round8b/entry-full-cycle
   scripts/boot/upload-entry.sh round8b-round7-24ef187b30 build/boot/round8b/entry-round7
   scripts/boot/install-device.sh --check
   scripts/omarchy/ssh.sh 'readlink /boot/linux-a9/default; cat /proc/cmdline'
   ```

   Compare complete cmdlines; retain the exact write window, three-argument
   warm contract, `panic=0`, cold-reset guard, pstore/crash reservations and
   90-second root/120-second outer deadlines. Entries reuse the exact Round 8
   rescue-capable initramfs, tested config and j71t DTB.

3. First test a single transition, then the matching handoff cohort:

   ```sh
   scripts/boot/warm-chain.sh round8b-handoff-24ef187b30 1 build/boot/round8b/device/handoff-entry
   scripts/boot/warm-chain.sh round8b-handoff-24ef187b30 20 build/boot/round8b/device/handoff-chain
   ```

   Require the round8b release suffix in `uname -r`, the correct entry/changed
   boot ID, checked DART reinit, health `result 0 (passed)`, usable NVMe root,
   both CPUs/links and a successful 64 MiB direct read. Require absence of the
   old `blk_validate_limits` warning. The transition tests incoming
   normalization; later matching jumps test outgoing Round 8b handoff too.
   An inconclusive gate is a failed diagnostic sample even if root works;
   its recovery count must remain zero. The chain script stops on failure.

4. If handoff passes, use the same full-cycle comparison and crossover:

   ```sh
   scripts/boot/warm-chain.sh round8b-full-cycle-24ef187b30 1 build/boot/round8b/device/full-cycle-entry
   scripts/boot/warm-chain.sh round8b-full-cycle-24ef187b30 20 build/boot/round8b/device/full-cycle-chain
   scripts/boot/warm-chain.sh round8b-handoff-24ef187b30 1 build/boot/round8b/device/handoff-crossover
   scripts/boot/warm-chain.sh round8b-handoff-24ef187b30 5 build/boot/round8b/device/handoff-confirm
   ```

> Private graphics-dependent excerpt omitted from this source snapshot.

5. At the first failure, stop and preserve incoming dmesg/netconsole, the
   host-only snapshot and `/sys/fs/pstore/*` plus `/run/linux-a9/pstore/*`
   before another jump or cold recovery. The rescue prints incoming logs on
   `/dev/console`. Do not repeatedly mount/unbind/rebind or issue exploratory
   endpoint reads; use the established Round 8 evidence procedure. No syscfg,
   NVRAM, boot-firmware or namespace 2–7 writes are authorized by this plan.

## Round 8c — local slot retirement must not reset a healthy controller

Round 8b's cold aop/ctri-r4 → handoff entry **passed**: checked DART rebuild,
health gate, mounted root and 64 MiB direct read at 406 MB/s, zero bad messages.
Its first same-entry handoff **failed**. Evidence is committed in
[round8b-handoff-chain-fail](../release/OMISSIONS.md). In particular,
`netconsole-rescue.txt` replays older rescue logs inside newer timestamps.
Use the innermost timestamp and boot context; repeated replay lines are not
additional boots or faults.

### Slot 14: the known failure and the missing observation

The failing incoming boot reports checked DART reinit, CC=0/RDY=0, NVMMU
rebuild, enable/RDY, an I/O queue, `health gate ... result 0 (passed)` at
**12.148885**, partition discovery at **12.186019**, then
`ANS DMA slot 14 quarantined: -19; scheduling reset` at **12.213714**.
The `err 0x500 valid 0` transaction record is not a DART fault. The later
RDY deadline, Identify timeout, one full-cycle recovery and terminal
`state 6 online 0 ... recovery_used 1` follow the software-triggered reset;
they do not justify that first reset retroactively.

The exact call chain is `nvme_pci_apple_ans_unmap_data()` →
`nvme_pci_apple_ans_free_req()` →
`apple_ans_nvmmu_map(dev, 14, NULL, 0, false, NULL)`.
The same unmap routine is used on completed requests and on failed mapping
cleanup, so the old message alone does not distinguish those callers.
In the 8b implementation, **every possible `-ENODEV` in this API is a local
host/port guard before TCB access or invalidation**:

- No host device/parent or no H9P host data.
- An invalid PCI endpoint/bus passed to `apple_h9p_pcie_device_port()`.
- A result other than port 0, or port 0 absent from `enabled_ports`.

The host and PCI device had just handled the successful gate and partition
scan. The particularly fragile check is the port resolver: it rereads
root-port `PCI_PRIMARY_BUS` secondary/subordinate ranges for each map and
retirement. Those registers are mutable routing state, not a stable owner.
The late Wi-Fi worker concurrently powers its port and calls
`pci_rescan_bus()`; the PCI scanner can rewrite bridge routing during scan.
The Wi-Fi DART reinit follows at 12.250700. A transient failed range lookup
is therefore the leading source-based explanation, **not a captured register
observation**. The evidence does not log the rejected range, resolved port or
`enabled_ports`, so it cannot prove which one of those guards returned -19.

**Slot 14 is an index in the new kernel's shared 36-entry FlatDMA pool.**
It is not NVMe queue ID 14, CID 14, NSID 14, or proof that an old-kernel
request survived. The pool is allocated in the new kernel and its bitmap
and round-robin cursor reset during preinit. Admin and I/O requests share
it. The old quarantine message records no qid/CID/opcode/NSID, so identifying
it as a particular partition read or namespace Identify would invent data.
The later `timeout tag 7 cid 0x2007 qid 0 opcode 0x6` belongs to the reset's
subsequent Identify, not proven to be the original slot-14 owner.

### Round 8c change and recovery policy

Kernel commit: **`bd24e6785e050003481478d798a76695006f7341`**, only on
`a9/boot`. The one incremental export is in `patches/boot/round8c/`.

Port ownership now walks `pci_bus` parents to the root-port slot, using the
existing config-guard topology helper. It reads no ECAM/routing registers;
NVMMU operations and MSI allocation use the same owner. Host lookup and
port rejection log the slot and failing branch/port/enable mask before
hardware access.

Each FlatDMA allocation records qid, CID, opcode, NSID and whether it was
successfully published to the NVMMU. Allocation resets that metadata. A
failed allocation/bvec/host-map path releases an unpublished slot directly,
without attempting invalidation. For a published slot:

- Successful retirement releases the slot normally.
- A local lookup/argument/unsupported error retains the slot, logs its owner
  and `local retirement error; no reset`, and never calls `nvme_reset_ctrl()`.
  Retention avoids reusing a possibly cached TCB; a clean boot sample still
  fails on this diagnostic. It is not a successful retirement.
- `-ETIMEDOUT` from the actual invalidate FIFO poll, or `-EIO` from its
  already-latched poison, retains the slot and can schedule reset. These
  specific return sites establish a hardware DMA failure. No other errno in
  this retirement path authorizes reset. The existing recovery quota and
  timeout/fault safeguards remain in place.

### Why CC.EN remained set; incoming full bring-up decision

The outgoing 8b shutdown did **exactly what handoff mode requested**.
The innermost outgoing log shows FIFO drain result 0 at 41.041929,
`starting NVMe SHN` at 41.045640, `SHST complete; CC.EN retained mode 1`
at 41.188446, then `kexec handoff; CC.EN/SHST retained` at 41.285554.
The subsequent generic "PERST asserted ..." line covers the other ports;
`apple_h9p_pcie_park()` explicitly excludes ANS port 0 from that Round 7
helper in handoff mode. It does not prove port 0 was reset.

`nvme_disable_ctrl(..., true)` sets SHN_NORMAL while retaining CC.EN. The
ANS callback checks SHST completion and clears PCI bus mastering, but does
not clear CC.EN. CC=0x464001 contains EN and SHN_NORMAL; CSTS=0x9 is
**RDY (0x1) | SHST complete (0x8)**, with CFS clear. This was a deliberate
Round 8/8b contract, not a missed call to the old Round 7 CC-clear callback.

The old incoming L0 predicate proved a trained link, not ownership or safe
reuse of all inherited DMA state. Its initial normalization did reach a
passed gate in this sample, so the evidence does not prove that fast path
caused the local -19. Nevertheless it is not a validated same-entry path.
**Round 8c removes it:** all non-round7 incoming ANS boots use the existing
full port sequence, including same-entry handoff jumps. PERST isolates the
endpoint before port reset/refclk/power sequencing and installation of new
NVMMU tables. Checked DART reinit, CC=0/RDY=0, NVMMU rebuild, BME/enable and
the gate still run. This matches the successful entry's incoming sequence.

Outgoing handoff continues to require drained I/O, empty FIFO, certified
SHST and BME off, and still retains CC.EN. Full-cycle additionally performs
its existing outgoing DART-inactive/port-power teardown. Both now normalize
the incoming port; the one alternate recovery still has the same quota.
The Round 7 control, read-only namespace policy, exact write window,
config/DTB, rescue initramfs, pstore, deadlines and other peripherals stay.

### Offline reproduction and provenance

```sh
mkdir -p build/boot/round8c
scripts/boot/test-round8c-regression.sh > build/boot/round8c/regression.log 2>&1
scripts/boot/test-ans-handoff-offline.sh > build/boot/round8c/handoff.log 2>&1
scripts/boot/test-ans-io-offline.sh > build/boot/round8c/ans-io.log 2>&1
scripts/boot/test-ans-offline.sh > build/boot/round8c/ans.log 2>&1
scripts/boot/test-recovery-offline.sh > build/boot/round8c/recovery.log 2>&1
scripts/boot/test-warm-peripherals.sh > build/boot/round8c/peripherals.log 2>&1
scripts/boot/test-offline.sh > build/boot/round8c/policy.log 2>&1
scripts/boot/test-update-offline.sh > build/boot/round8c/update.log 2>&1
scripts/build-kernel.sh --tree boot > build/boot/round8c/build-final.log 2>&1
TREE=boot scripts/boot/check-build.sh > build/boot/round8c/check-build.log 2>&1
scripts/boot/export-round8c.sh
scripts/build-kernel.sh --run boot 'git apply --check --reverse /work/patches/boot/round8c/*.patch; scripts/checkpatch.pl --strict --no-tree /work/patches/boot/round8c/*.patch' \
  > build/boot/round8c/checkpatch-export.log 2>&1
scripts/boot/build-round8c.sh > build/boot/round8c/integration-build.log 2>&1
scripts/boot/test-fdt-merge.sh build/boot/round5b/entry/dtb \
  build/kernel-bootr8c-check/dtbs/s8003-j71t.dtb > build/boot/round8c/fdt.log 2>&1
scripts/boot/test-ans-handoff-offline.sh --source /src/wt/bootr8c-check \
  > build/boot/round8c/handoff-integration.log 2>&1
scripts/boot/test-ans-io-offline.sh --source /src/wt/bootr8c-check \
  > build/boot/round8c/ans-io-integration.log 2>&1
python3 -B -I scripts/boot/prepare-round8c-entries.py
shasum -a 256 -c build/boot/round8c/SHA256SUMS
```

The new ASan/UBSan fixtures execute the production allocation/map/unmap/free
functions with slot 14, failed allocations/bvec/host maps, actual published
requests, local retirement errors and confirmed hardware errors. They verify
unpublished slots are released, local failures retain slots with zero resets,
owner diagnostics are accurate, and FIFO timeout/poison still quarantine and
schedule reset. The topology fixture executes the real parent walk for both
ports and a nested bridge, permits no `readl()` implementation, varies bus
numbers and rejects invalid endpoints. Existing production tests cover
finite full electrical bring-up for retained/on/off port states, unchanged
outgoing SHST gates, DART/health error classification, and bounded recovery.

Final offline validation: **92 tests passed**, one optional real update-bundle
case skipped. The 12 handoff and eight I/O tests also pass against the exact
detached integration. Both expected negative controls reproduce the old 8b
reset/cleanup bugs, including slot-14 `-ENODEV` retirement. Both full builds
pass: **104 standalone / 105 integration DTBs**. Strict exported-patch
checkpatch reports **0 errors, 0 warnings, 0 checks**; reverse application,
clean `a9/boot`, exact overlay equality, required config and all 67 manifest
file checksums pass. Entry cmdlines equal 8b's except for their entry names.
The tested integration config, j71t DTB and rescue initramfs are unchanged.

Packaged release:
`7.3.0-rc1-a9-boot-r8c-bd24e6785e-gd5822a7f71cd-dirty`.
Image.gz SHA-256:
`413f8dbe0775415cccc567ca629ed14fce8e7168a670d1d8afcede1d4028b626`.
DTB SHA-256:
`fcc22884a3ada4fd2309b946e2937c92b9900e5130d981a40bfa5b20f03af75c`.
Initramfs SHA-256:
`412b16a6722a8eec5fa86e8d30456c1f0161387491644a3f9e629547af41e6b7`.
The three entries have identical payload bytes. `build.json` and SHA256SUMS
bind the final source, release, export, scripts/fixtures, logs and entries.
Only the `a9/boot` kernel commit was created; workspace docs/scripts/export
remain uncommitted. No device access, hardware validation or push occurred.

### Ordered orchestrator-only device plan: DFU, entry, 20-chain

The iPad needs **physical DFU first** after the 8b failure. Do not kexec out
of that rescue into this trial. These commands are prepared for the
orchestrator; none were executed by the Round 8c builder.

1. Verify the build and start a dedicated capture terminal:

   ```sh
   shasum -a 256 -c build/boot/round8c/SHA256SUMS
   scripts/omarchy/netconsole.py build/boot/round8c/device-netconsole.log
   ```

   Put the device in DFU using the established Power/Home procedure, then
   cold-boot the same known aop/ctri-r4 baseline used for the passed entry:

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

2. Stage the immutable Round 8c entries, leaving the default unchanged:

   ```sh
   scripts/boot/install-device.sh
   scripts/boot/upload-entry.sh round8c-handoff-bd24e6785e build/boot/round8c/entry-handoff
   scripts/boot/upload-entry.sh round8c-full-cycle-bd24e6785e build/boot/round8c/entry-full-cycle
   scripts/boot/upload-entry.sh round8c-round7-bd24e6785e build/boot/round8c/entry-round7
   scripts/boot/install-device.sh --check
   scripts/omarchy/ssh.sh 'readlink /boot/linux-a9/default; cat /proc/cmdline'
   ```

   Verify complete cmdlines retain `nvme.write_window=1:3146240-7812095`,
   NVMe root, the three-argument warm contract, `panic=0`, cold-reset guard,
   pstore/crash reservations, 90-second root wait and 120-second outer
   deadline. The three entries share the same kernel, tested j71t DTB and
   unchanged rescue initramfs; only entry identity and ANS mode differ.

3. Run the entry transition first; continue only on PASS:

   ```sh
   scripts/boot/warm-chain.sh round8c-handoff-bd24e6785e 1 build/boot/round8c/device/handoff-entry
   ```

   Require the embedded `-a9-boot-r8c-bd24e6785e` release, correct entry/new
   boot ID, **`Apple ANS: handoff full port bring-up`**, checked DART reinit,
   controller stopped/rebuilding DMA, NVMMU rebuilt, gate
   `result 0 (passed)`, partition scan, usable RW root, both CPUs/links and
   successful 64 MiB direct read. `handoff already-enabled` must be absent.
   Preserve this entry's logs separately from the same-entry cohort.

4. If entry passed, run exactly the matching 20-chain cohort:

   ```sh
   scripts/boot/warm-chain.sh round8c-handoff-bd24e6785e 20 build/boot/round8c/device/handoff-chain
   ```

   Require the same full-bring-up and health evidence on **every** jump,
   including chain boot 1. No quarantine or local NVMMU lookup rejection is
   acceptable even if root remains usable: a local-error sample must also
   show no software-triggered reset/recovery. Any timeout, reset, alternate
   recovery, polled completion, poisoned slot/FIFO, valid-bit DART fault or
   checked flush/readback failure fails the cohort. A plain pre-ack
   `err 0x500 valid 0` does not. `warm-chain.sh` stops at the first failure
   and now checks Round 8c release/full-bring-up plus local quarantine logs.
   Keep the staged full-cycle and round7 entries as explicit controls;
   crossover boots are separate samples and are not part of these 20.

5. On the first failure, stop. Save the host-side pre-jump snapshot,
   netconsole, incoming dmesg and `/sys/fs/pstore/*` plus
   `/run/linux-a9/pstore/*` before another jump or DFU. The rescue's incoming
   replay may nest older timestamps: preserve the raw capture. New slot
   diagnostics distinguish the failed host lookup or port/enable check and
   record qid/CID/opcode/NSID/`mapped` for quarantined published requests.
   A local quarantine must not be followed by reset just because of that
   error. Do not repeatedly mount, unbind/rebind or probe endpoint MMIO to
   collect more evidence. Use the established host-only ANS state helper.

Default promotion is not part of this trial. Namespace 2–7, syscfg, NVRAM
and boot-firmware write guards remain intact.

## Round 8d — disabled controller before full port bring-up

The 8c entry passed again, but its first same-entry jump hard-hung after
port tunables with outgoing CC.EN/SHST retained. Round 8d restores a certified
EN/SHN-clear, RDY=0 shutdown before port cycling and adds bounded host polls
and per-step breadcrumbs. See [round8d.md](../release/OMISSIONS.md) for the raw evidence
boundary, tri32/iBoot distinction, firmware/MMIO limits, offline results and
the exact DFU → stage → entry → 20-chain commands. The incremental kernel
commit is `9c11ab8866e1bb6b66f0893f55a8337324e9dfc3`, only on `a9/boot`.
