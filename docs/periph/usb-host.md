# J71t USB host: morning handoff

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Prepared 2026-10-25, entirely offline. **The payload builds, but this is a
manual-role milestone, not verified plug-and-play.** No device was contacted.
The actual Camera Adapter, its accessory power/routing, a keyboard, and a flash
drive still need the morning test. Do not promise they will enumerate merely
because EHCI/OHCI root hubs appear.

## Artifacts and implementation

> Private graphics-dependent excerpt omitted from this source snapshot.

The new integration enables SCSI disks, USB storage/UAS, VFAT/exFAT/NTFS3/ext4,
MSDOS/GPT partitions, NLS and USB HID/evdev. It restores the PHY's `.set_mode`
hook for its optional runtime-PM operations too. The packaged command line
keeps optional PHY runtime PM off, following the previously tested setup.

The Tristar supplier now references the Lightning role switch. It reports zero
input allowance in host mode and **disables analog brick classification on this
shared port**, including in device mode, avoiding a race that could divert the
host data pins to the PMIC ADC. Digital gadget current grants still work in
device mode. Analog wall-charger current classification is therefore a known
tradeoff of this trial; powered-adapter charging is not claimed.

EHCI and OHCI each receive the ADT's **100 mA software power budget**. A drive
requesting more can be refused. This is enumeration policy, not a hardware
current limiter. Use a low-power keyboard/drive first; try a correctly powered
Camera Adapter or downstream powered USB hub if the drive exceeds the budget.

## Hardware conclusions and remaining gap

Sources checked: the live iOS ADT in
`build/firmware/private/IODeviceTree.txt`, decoded `docs/adt/j71t.txt`, the saved
device-mode IOService dump, the shared Ghidra kernelcache
`kernelcache.release.iPad6,11_12` (SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`),
and the actual Hoolock/tri52g kernel sources.

- **Separate controllers, one USB2 PHY.** DWC2 at `0x20c100000`, AIC DT IRQ
  214, is device-only (`GHWCFG2.OP_MODE=4`, previously measured). Host is
  EHCI at `0x20c200000`, IRQ **215**, plus OHCI at `0x20c300000`, IRQ **216**.
  These are the S8003 IRQ cells, not the inherited A7's 163/164. No DWC2
  dual-role setting can manufacture its missing host hardware.
- PHY: `0x20c000030`, length `0x20`, under `ps_usbctrl`. USB_CTL is
  `0x20e0d8000`, length `0x1000`. The USB complex remap block at
  `0x20c900000` belongs to the existing bus driver: host DMA remaps are
  `0x108` (32-bit bus addresses into DRAM at `0x8_00000000`). EHCI/OHCI glue
  words are `0x20c000000` / `0x20c000008`.
- Power/clock hierarchy already exists: `ps_usb` → `ps_usbctrl`
  (PMGR `+0x80258`) → `ps_usb2host0` (`+0x80260`) →
  `ps_usb2host0_ohci` (`+0x80268`), with PMGR based at `0x20e000000`.
  DWC2 uses `ps_usbotg` (`+0x80290`) and the existing reference clock.
  The ADT complex lists clock gates `[8,105,10,101,12,14,91,92]`, clock ID
  277. The Linux host children use the existing PMGR domains; there is no
  new guessed host clock frequency or clock-controller driver.
- iOS publishes device criteria 3 or host criteria 4, terminates the device
  nub, then publishes EHCI/OHCI. `AppleEmbeddedUSBArbitrator` at
  `fffffff0060770f8` contains that choice. PHY `enableHostMode` at
  `fffffff006cbfd58` selects USB_CTL bit 0, writes host CFG0/CFG1
  `0x37373b03` / `0x00020c04`, clears trim, and pulses reset 20 µs + 1 ms.
  The inherited Linux role driver reproduces this, unbinding/rebinding the
  gadget and creating/destroying the host platform devices.
- Lightning IC is **CBTL1610 Tristar**, I2C0 `0x1a`, revision **`0x8a`**.
  Its ADT interrupt is AP GPIO 72. The existing supplier polls; this work
  does not acknowledge the incompletely understood event register `0x0d`.
  `0x0c & 3` indicates incoming VBUS, not host identity or an output grant.
  Full accessory identity/authentication uses the separate AID/ID bus.
- Ghidra reconfirmed `IOAccessoryPortUSB` vendor OUT `0x40/0x51`, zero
  payload, mask-checked value/index, scheduling the switch (normally 50 ms).
  The capability IN request `0xc0/0x53` requires **four bytes**, value/index
  zero; older notes' two-byte description is wrong. That interface is
  `IapOverUsbHid`, absent from our gadget. This establishes iOS code behavior,
  not a capture proving what this Camera Adapter sends under Linux.
- Accessory power knobs already exist: PMIC LDO5 `0x0310` bit 0 and switch
  `0x2603` (the ADT dock's `acc_pwr=pmuL(5)` and
  `acc_sw_en=pmuL(0x80000005)`). The LDO table is a low-voltage accessory
  rail, **not evidence of a 5 V USB source**. Voltage selector `0x2600` is
  read-only in this implementation. Tristar `setVBUSSink` at
  `fffffff005ff5214` changes `0x05` bit 0, settling 18 ms, and iOS refuses
  it while ID_BUS is in reset. A **sink** gate does not demonstrate VBUS
  output. Whether the adapter powers up, routes data, and provides downstream
  5 V with these gates remains unmeasured. No new guessed boost register is
  written.

The two newly understood Ghidra functions were renamed/commented and saved.
The earlier [analysis](ds-usb-host.md) and [round-1 plan](usb-host-test-plan.md)
contain more addresses; use the corrections above for PHY addresses, IRQs,
request length and the distinction between accessory power and 5 V VBUS.

The [Hoolock PHY source](https://github.com/HoolockLinux/linux/blob/hoolock/drivers/phy/apple/ausb.c)
and Pauli1Go's [dev](https://github.com/Pauli1Go/HoolockLinux/blob/dev/drivers/phy/apple/ausb.c)
and [touch](https://github.com/Pauli1Go/HoolockLinux/blob/touch/drivers/phy/apple/ausb.c)
branches still compile out host `set_mode`; the
[nvme DTS](https://github.com/Pauli1Go/HoolockLinux/blob/nvme/arch/arm64/boot/dts/apple/s5l8960x.dtsi)
provides the older controller template. Asahi provides the generic role-switch
framework and [TI/CD321x USB-C logic](https://github.com/AsahiLinux/linux/blob/asahi/drivers/usb/typec/tipd/core.c),
which is not a CBTL1610 Lightning accessory driver. This trial reuses our
already implemented `a9/usbhost` glue rather than treating those as automatic
Tristar support.

No automatic accessory detection is claimed. The previous device test's
`SIG=0x1c103` even with the Mac cable demonstrates why bit 16 alone is not
an adapter detector. A correct automatic path needs real adapter captures and
ID-bus/routing reconstruction or the verified vendor protocol. The ID-bus
protocol half now exists: [lightning-idbus.md](lightning-idbus.md).

## Offline checks completed

Run again with `scripts/periph/check-usb-host.sh` and
`scripts/periph/test-usb-host-install.sh`.

- Full Image.gz + **105 DTBs** built with debug information disabled;
  clean kernel working tree and original kexecfix ancestry checked.
- W=1 compilation of role switch, PHY, Tristar, EHCI and OHCI objects passed.
  Strict checkpatch: 0 errors, 0 checks, 2 integration warnings (separate
  binding commit / MAINTAINERS), recorded in `build/usb-host/check-kernel-final.log`.
- Changed charger/role bindings and compiled J71t DTB validate with the
  existing dtschema installation. The full schema cache still reports the
  pre-existing mfd/apple,pmic and pci/apple,t8010-pcie parse warnings;
  optional yamllint is absent. No diagnostic was reported for the two
  selected bindings. `build/usb-host/offline-checks.log` records this.
- 14 semantic DT merge tests passed against the real tri52g → USB-host
  pristine DTBs and synthetic m1n1 fixups, including framebuffer/PSCI and
  phandle renumbering. These are offline fixtures, not this boot's live FDT.
- Payload extraction, config requirements, compiled MMIO/IRQ cells, entry
  hashes, initramfs identity and the command-line limit pass.
- 10 userspace safety tests pass: USB ancestry, inherited partition transport,
  exclusion of internal NVMe, no role drop before unmount, preserving host
  on busy/unknown mounts, unique-file readback and cleanup.
- Disposable ALARM-container installation twice passes ownership, autostart
  forwarding and systemd unit validation. Its package queries are mocked;
  this does not exercise a real UDisks mount or Hyprland.
- Offline package hashes and all 12 included AArch64 ELF files pass the
  16K-page audit. Packages were resolved against the saved rootfs image,
  not today's live pacman database; pacman still checks dependencies on install.

## Morning: install and warm-kexec over the tether

The following commands are **for the orchestrator** on the Mac. Keep a recovery
console available as in [warm-reboot.md](../boot/warm-reboot.md); ANS warm-boot
recovery is an existing hardware risk even with keep-power. No default entry
change is needed for this test.

```sh
cd .
export OMARCHY_HOST=172.16.42.1
S=scripts/omarchy/ssh.sh
"$S" 'findmnt -no SOURCE,FSTYPE /; uname -r; cat /proc/cmdline; sudo -n linux-a9-kexec status'
# Require /dev/nvme...p2 root, working kexec/PSCI, and the actual running
# entry staged with its matching pristine DTB. If provenance is missing,
# stage the original running payload per warm-reboot.md before proceeding.
# Never substitute this trial DTB for the running entry's old DTB.

scripts/omarchy/install-usb-host.sh
ENTRY=$(cat build/usb-host/entry-name)
ENTRY="$ENTRY" scripts/boot/kexec-host.sh "$(cat build/usb-host/payload-path)"
"$S" 'uname -r; cat /proc/sys/kernel/random/boot_id; findmnt -no SOURCE /; cat /sys/class/usb_role/lightning-role-switch/role; cat /sys/module/pcie_apple_h9p/parameters/ans_keep_power'
"$S" 'systemctl --user start linux-a9-udiskie.service; systemctl --user --no-pager status linux-a9-udiskie.service; sudo -n systemctl is-active udisks2; sudo -n journalctl --flush'
```

Expect entry `usb-host-0cfbadcd4f`, a new boot ID, NVMe root, role `device`,
keep-power `Y`, and both automount services active. If the new kernel does not
boot, recover with the established tethered DFU/pongo flow and the saved
payload; increasing USB trial time cannot fix an ANS failure.

The install script transfers the prepared `build/usb-host/pkgs` bundle first.
Only four missing packages (usbutils, fuse2, libntfs-3g, ntfs-3g; about 500 KiB)
were needed against the saved image. UDisks2/udiskie/Nautilus/GVfs were already
in it. If the bundle is absent or today's image lacks other dependencies,
the script attempts normal pacman installation of missing packages without
refreshing databases; resolve a failed transaction before unplugging.

The install adds the launcher entry **USB Drives** (Nautilus `computer:///`),
a single `linux-a9-udiskie` user service tied to the graphical session, and a
small `/usr/local/bin/udiskie` wrapper forwarding Omarchy's stock autostart to
that service. It stops an already running stock udiskie once during install.
The narrow polkit rule permits the default user `linux` to mount USB
filesystems from its service/SSH context; it does not grant internal disk,
formatting or partitioning actions. Internal NVMe and loop devices are ignored.
USB disks mount below `/run/media/linux/`; ext4 ownership remains the drive's
normal Unix ownership. No test changes permissions on the owner's files.

For a full offline package re-fetch on the Mac:

```sh
scripts/omarchy/fetch-live-pkgs.sh --image "$PWD/build/omarchy/rootfs.img" \
  --out "$PWD/build/usb-host/pkgs" \
  udisks2 udiskie nautilus gvfs polkit usbutils exfatprogs dosfstools ntfs-3g
python3 -B -I scripts/periph/check-usb-host-packages.py
```

## Prove Wi-Fi, then attach adapter and drive

The project records working BCM4355 Wi-Fi, usually named **wld0**, and
reconnect after suspend. That does not prove this trial's current association
or reachability. Cached `build/omarchy/wifi-host` was `192.0.2.168`; it may
be stale. The helper below uses a saved NetworkManager Wi-Fi profile and
compares boot IDs through **independent Wi-Fi and tether SSH connections**.
It writes the cache only after that proof. It prints no Wi-Fi password.

```sh
scripts/periph/usb-host-wifi.sh
export OMARCHY_HOST=$(cat build/omarchy/wifi-host)
"$S" 'ip -br address; cat /proc/sys/kernel/random/boot_id; systemctl --user is-active linux-a9-udiskie.service'

# Detach the trial from SSH. Twenty seconds to move the cable, then a
# three-minute host window, followed by unmount and gadget restoration.
"$S" 'sudo -n linux-a9-usb-host arm none 20 180'
```

**Now tell the owner:** unplug the Mac cable from the iPad, plug in the Apple
Lightning-to-USB Camera Adapter, then the low-power keyboard or test flash
drive. Keep Hyprland awake. Open **USB Drives** in the app launcher or Nautilus
and look for the drive in its sidebar. No manual mount command belongs in the
automount pass criterion.

Over the independently verified Wi-Fi SSH link:

```sh
"$S" 'sudo -n linux-a9-usb-host status; sudo -n dmesg | tail -100'
"$S" 'lsusb; lsusb -t; lsblk -o NAME,TRAN,SIZE,FSTYPE,LABEL,MOUNTPOINTS; findmnt -r -o SOURCE,TARGET,FSTYPE,OPTIONS'
"$S" 'journalctl --user -b -u linux-a9-udiskie --no-pager -n 60; sudo -n journalctl -b -u udisks2 --no-pager -n 60'
# Inspect lsblk and choose the actual USB filesystem, /dev/sda1 here.
# The checker rejects NVMe, regular files and mounts outside this user's
# /run/media directory. It writes/fsyncs/reads/deletes one unique 64 KiB file.
"$S" 'ipad-usb-check /dev/sda1'
"$S" 'cat /proc/bus/input/devices; cat /sys/class/power_supply/usb/{online,current_max}'
```

PASS requires actual EHCI high-speed or OHCI low/full-speed enumeration,
usb-storage/UAS → SCSI disk for storage, an automounted filesystem, a visible
drive in Nautilus, and `PASS user USB read/write`. For the keyboard, type
`echo usb-keyboard-ok` in the iPad terminal and press Enter; seeing a new HID
entry alone does not prove typing works. Host-mode input current should
settle to `online=0`, `current_max=0` — but only when **no** power is fed
into the USB3 Camera Adapter's Lightning input: with a charger attached there,
VBUS legitimately reaches the iPad while the data pins host, so `online=1`
with charging is a valid state, not a policy failure. Capture and compare
both.

To keep a successful host session instead of the timed return:

```sh
"$S" 'sudo -n linux-a9-usb-host confirm'
# Later: the helper unmounts USB filesystems before switching back.
"$S" 'sudo -n linux-a9-usb-host device'
```

Unmount must succeed before disconnecting the drive/adapter. If a filesystem
is busy or its mount state is unreadable, the helper cancels the kernel revert
and **keeps host mode** to protect data. Close applications using the drive,
unmount in Nautilus or with `udisksctl`, then retry `device` from Wi-Fi or the
iPad terminal. Do not pull the drive merely because the three minutes expired.

## Power ladder and manual role fallback

If there are only root hubs and no attached USB device, keep the evidence and
retry these explicit accessory-power combinations, returning to device mode
between attempts. `pmu` and `both` are existing, unverified electrical knobs;
the default is `none`. With each trial, move the cable during its 20-second
delay. Stop escalating as soon as real enumeration works.

```sh
"$S" 'sudo -n linux-a9-usb-host device'
"$S" 'sudo -n linux-a9-usb-host arm pmu 20 180'
# If no attach, let it return, reconnect the Mac if necessary, then:
"$S" 'sudo -n linux-a9-usb-host device'
"$S" 'sudo -n linux-a9-usb-host arm both 20 180'
```

Try a powered downstream hub or the USB3 Camera Adapter with its power input
if a drive is over budget. Do not interpret 100 mA rejection as missing SCSI
support, or respond by raising the budget without a verified power source.
If a low-power keyboard still never connects with any gate choice, the next
task is adapter/AID-bus/routing capture, not more filesystem packages. The
protocol half of that task is now documented in
[lightning-idbus.md](lightning-idbus.md) (IDBUS wire protocol, CRC8, GetID,
ACCx/Dx routing, power handshakes, and the ordered capture/Ghidra plan);
what's missing is the Camera Adapter's own ID and the Tristar I²C command
sequence.

Raw manual fallback, **over Wi-Fi or the iPad terminal, after removing the Mac
cable** (first unmount any USB filesystems):

```sh
R=/sys/bus/platform/devices/20e0d8000.usb-role-switch
echo 300 | sudo -n tee "$R/revert_timeout"
echo none | sudo -n tee "$R/vbus_gates"       # or the measured working gate
echo host | sudo -n tee /sys/class/usb_role/lightning-role-switch/role
# Optional, after real enumeration succeeds:
echo 1 | sudo -n tee "$R/confirm"
# When finished, after unmounting:
sudo -n linux-a9-usb-host device
```

The raw unconfirmed switch has a kernel timer; it does not unmount drives
before expiring. Use the higher-level trial helper for storage tests. Do not
write configfs `UDC` while host mode owns the PHY. Detailed register capture,
if needed after the ordinary checks, is
`sudo cat /sys/kernel/debug/apple-usb-role/regs`; it reads MMIO as well as I2C.

## If Wi-Fi is unavailable

Run the same `arm none 20 180` command **over the tether**, then let the owner
move the cable. The detached service needs neither SSH nor Wi-Fi. It records
role, lsusb tree, lsblk/mount state, IP state, kernel messages and UDisks logs
to **`/var/log/linux-a9-usb-host/<UTC>-<boot-id>/` on NVMe**, with
`latest-status.txt` at the top level. Ten trial directories are retained.
Persistent journald is capped at 128 MiB / seven days. The trial inhibits sleep.

The owner can open USB Drives and run `ipad-usb-check /dev/sda1` in a terminal;
its PASS line persists at `~/.local/state/linux-a9/usb-host-check.txt`.
After about four minutes, the helper normally unmounts and restores gadget
mode. Reconnect the Mac cable. If cleanup was interrupted, the kernel fallback
is approximately two minutes later. The busy-filesystem exception above takes
precedence: use touch/OSK on the iPad to close files and run `device`.

Retrieve evidence after re-tethering:

```sh
export OMARCHY_HOST=172.16.42.1
"$S" 'sudo -n cat /var/log/linux-a9-usb-host/latest-status.txt'
"$S" 'sudo -n tar -C /var/log -czf - linux-a9-usb-host' > build/usb-host/device-logs.tgz
"$S" 'sudo -n journalctl -b -u linux-a9-usb-host-trial --no-pager' > build/usb-host/device-trial.txt
"$S" 'journalctl --user -b -u linux-a9-udiskie --no-pager' > build/usb-host/device-automount.txt
"$S" 'cat ~/.local/state/linux-a9/usb-host-check.txt 2>/dev/null || true'
# If a reboot intervened, inspect saved boot IDs before selecting old logs:
"$S" 'sudo -n journalctl --list-boots'
```

This work never accesses syscfg, NVRAM or boot-firmware storage, and never
controls qemu/QEMU processes. Installation targets the running NVMe root only;
no offline filesystem image was mounted or modified.

## 2026-10-26 first hardware test (camera adapter, flash drive)

DFU → pongoOS → `usb-host-0cfbadcd4f` cold. Automount installed, Wi-Fi SSH proven
(192.0.2.168). Host mode was armed three times with gates `none`, `pmu` and `both`, with
the Lightning camera adapter and a USB flash drive attached, and with a charger on the
adapter's Lightning input for at least part of the test.
- Every attempt: EHCI and OHCI root hubs register, but **no device enumerates**. The EHCI
  port reads `status 501000 ACK POWER sig=se0` (no line state). The OHCI root hub
  runtime-suspends. `usb` power supply `online=0 current_max=0` in host mode.
- In device mode, charging through the adapter works (`online=1 current_max=500000`). In
  host mode it stops.
- Reading: the role switch and controllers work. The adapter is not identified, so it
  does not route or power its USB port. As planned, the next task is the
  adapter's ID-bus identification and Tristar routing ([lightning-idbus.md](lightning-idbus.md)),
  not more power knobs. A secondary check is that OHCI runtime suspend does not mask connect events.
Logs: `logs/usb-host-20261026/`.

## Second trial: Lightning ID-bus identify, then host (usb-host2)

Prepared 2026-10-26, offline only; `hardware_tested=false`. This implements
[ds-lightning-host-idbus.md](ds-lightning-host-idbus.md) §9 as a **manual**
GetID. Nothing runs on its own, and the role switch is unchanged.

**Artifacts**
- Kernel branch **`a9/usb-host2`**, commit **`9977e33ee6`** (one commit on
  top of `a9/usb-host` `0cfbadcd4f`). Patch:
  `patches/usbhost2/0001-power-supply-apple_tristar-add-manual-Lightning-ID-b.patch`.
- Entry **`usb-host2-9977e33ee6`**: `build/usb-host2/entry/`, payload
  `build/boot/m1n1-linux-usb-host2-9977e33ee6.bin`, manifest
  `build/usb-host2/manifest.json`. The base is the same as usb-host's
  (tri52g/kexecfix cmdline + initramfs, m1n1 `psci=1`), with exactly one
  `pcie_apple_h9p.ans_keep_power=1`. The cmdline is 870 bytes. The packager
  asserts that the config and all DTBs are byte-identical to the
  usb-host build, so only the kernel image differs. Debug info is off.
- Build: `scripts/periph/build-usb-host2.sh`. Package only:
  `python3 -B -I scripts/periph/package-usb-host2.py`. Offline tests:
  `scripts/periph/test-tristar-idbus.sh` (the kernel's own ID-bus code
  against a fake CBTL1610: CRC8 vectors incl. `74 00 02 → 1f`, CRC-8/MAXIM
  check `0xa1` and our Mac cable's `75 10 0c 00 00 00 00 → 66`; the 16-field
  decode of all known IDs; the exact register order; one reorient at most;
  0x0a echo; an injected I2C error at every op stops all later writes; the
  power handshake) and `python3 -B -I scripts/periph/test-usb-host.py`
  (the helper's identify ordering).

**What `identify` does** (`drivers/power/supply/apple_tristar_idbus.h`, under
the supplier mutex):
1. Check revision `0x8a`. Read the baseline `0x0c 0x0d 0x05 0x01 0x02 0x17
   0x0a` and the 8 bytes the FIFO retained.
2. If `0x05` bit 3 (ID-bus reset hold) is latched, clear only that bit
   (bus-locked read-modify-write) and wait 20 ms.
3. Write `{74 00 02}` to FIFO `0x20`, then `0x41 = 0x0c`, then `0x40 = 0x05` (GO).
4. Poll `0x0d` every 10 ms for at most 100 ms. Completion is
   `(0x0d & 0x38) == 0x10` or the EVENT_DIGITAL_ID bit `0x08`. Then read
   8 bytes from `0x20` and require `0x75` plus a matching CRC8.
5. Retries: only a timeout (poll expiry or the `0x40` event) with VBUS
   present gets one reorient, `0x05 |= 0x02` with the VBUS-sink bit kept,
   then 20 ms and the iOS 100 ms CmdID gap, then one more GetID. A transfer
   error, a detach (`0x04`), a bad header or a bad CRC fails at once, but
   the raw frame is still published. A `0x0d` pin fault (`0x80`) is
   acknowledged by writing back exactly the `0x0a` value read. The worst
   case takes about 250 ms.

Every 1-byte register write is logged to dmesg (`idbus:`) and to the
transcript with the register's value read just before and just after. The
FIFO `0x20` load is logged with its bytes, bracketed by the baseline read of
the retained frame and the response read. Only `0x05`, `0x0a`, `0x20`, `0x40` and `0x41` are
ever written. `0x17`, `0x0e`, the mux registers, OTP/ESN, syscfg and NVRAM
are never touched. The FIFO uses plain I2C messages (pointer write + STOP,
then the read): the PA Semi controller has no SMBus I2C-block transfer, so
the doc's `i2c_smbus_*_i2c_block_data` would always fail. The
`IOAccessoryManagerSystemID` byte order stays `00 02`. `swap` is a
manual variant only.

debugfs `/sys/kernel/debug/apple-tristar/` (root only):
`identify` (write `identify`, optionally plus `swap` and/or `noreorient`;
the write fails with the error code), `identify_log`, `digital_id`,
`digital_id_decoded` (iOS's field names), `idbus_frame` (raw response +
retained frame + attempts), `reg_05`, `reg_0d` and `reg_0a` (cached, never a
live read). The parenthesised `0x0d` event OR ("seen this run") and the
`0x0a` acknowledgement count are cumulative since boot, despite the label.
The debugfs transcript is capped at 4 KiB. `dmesg | grep idbus:` is the
authoritative copy if it ends early. The optional power handshake: `power_handshake` +
`power_handshake_log`. It is refused unless module parameter
`apple_tristar_power.allow_power_handshake=Y` and an ID was verified in the
last 30 s. It sends `72` (expects `73`+4) and then one
`{70, state[1] & ~8, state[2]}` (expects `71`). Which state bytes iOS echoes
is inferred ([I]). The helper opens the parameter only around its own
trigger.

Helper (`linux-a9-usb-host`, workspace copy updated, **needs reinstall**):
- `arm GATE DELAY DURATION --identify[=default|swap|noreorient|swap-noreorient] [--power-handshake]`:
  the detached trial sleeps DELAY, **then** runs identify (the adapter is
  attached by then), the optional handshake (only if identify succeeded),
  and then arms host exactly as before. A failed identify is recorded and
  host is still armed. The results are `<trial>/identify.txt` and
  `/var/log/linux-a9-usb-host/latest-identify.txt`.
- `identify [--identify=MODE] [--power-handshake]`: immediate, no role
  change, logs `<UTC>-identify/`. With VBUS present, a timeout reorients
  the lanes, which can drop the tether. Use it **over Wi-Fi**, and with
  `--identify=noreorient` for the Mac-cable baseline.

### Owner test

The orchestrator runs these on the Mac. Keep the recovery console ready
([warm-reboot.md](../boot/warm-reboot.md)).

```sh
cd .
export OMARCHY_HOST=172.16.42.1
S=scripts/omarchy/ssh.sh
"$S" 'findmnt -no SOURCE,FSTYPE /; uname -r; cat /proc/cmdline; sudo -n linux-a9-kexec status'
scripts/omarchy/install-usb-host.sh          # deploys the new helper; idempotent
# Warm kexec (needs the running entry staged, as in the first trial):
ENTRY=$(cat build/usb-host2/entry-name)
ENTRY="$ENTRY" scripts/boot/kexec-host.sh "$(cat build/usb-host2/payload-path)"
# ...or cold: scripts/boot.sh pongo, then
#   scripts/boot.sh linux "$(cat build/usb-host2/payload-path)"
"$S" 'cat /proc/cmdline | tr " " "\n" | grep linux_a9.entry; cat /proc/sys/kernel/random/boot_id; findmnt -no SOURCE /; cat /sys/class/usb_role/lightning-role-switch/role; cat /sys/module/pcie_apple_h9p/parameters/ans_keep_power /sys/module/apple_tristar_power/parameters/allow_power_handshake; sudo -n ls /sys/kernel/debug/apple-tristar'
```

Expect `linux_a9.entry=usb-host2-9977e33ee6`, a new boot ID, an NVMe root,
role `device`, `Y` then `N`, and the ten debugfs files. Then prove Wi-Fi as
in the first trial:

```sh
scripts/periph/usb-host-wifi.sh
export OMARCHY_HOST=$(cat build/omarchy/wifi-host)
"$S" 'ip -br address; cat /proc/sys/kernel/random/boot_id; systemctl --user is-active linux-a9-udiskie.service'
# Baseline on the known Mac cable, over Wi-Fi, no reorient:
"$S" 'sudo -n linux-a9-usb-host identify --identify=noreorient'
```

The baseline should show `digital_id: 10 0c 00 00 00 00` and
`USB=2 ACCx=0 DB=0 MB=0 UART=0 PWR=0 HV=3 …` (iOS's live value for this
cable). That proves the register path before the adapter is involved. If
it fails, keep the transcript and still run the trial below: it is the
evidence that matters.

**The one command** (Wi-Fi SSH). It is the same 20 s / 180 s trial as
before, but identify runs at t = 20 s, right before host:

```sh
"$S" 'sudo -n linux-a9-usb-host arm none 20 180 --identify'
```

**Now tell the owner:** within 20 seconds, unplug the Mac cable and plug in
the Camera Adapter with the flash drive (and the charger on the adapter's
Lightning input, as in the first trial). Leave it attached. Identify runs
when the countdown ends, and host mode follows.

Read over Wi-Fi from about t = 25 s:

```sh
"$S" 'sudo -n cat /var/log/linux-a9-usb-host/latest-identify.txt'
"$S" 'sudo -n journalctl -b -u linux-a9-usb-host-trial --no-pager | tail -30'
"$S" 'sudo -n linux-a9-usb-host status; sudo -n dmesg | grep -E "idbus|usb [0-9]|ehci|ohci|sd " | tail -80'
"$S" 'lsusb; lsusb -t; lsblk -o NAME,TRAN,SIZE,FSTYPE,LABEL,MOUNTPOINTS'
```

Then continue with the first trial's PASS checks (Nautilus, `ipad-usb-check`,
`confirm`/`device`). How to read the results:

| identify result | enumeration | next run (each in its own `arm`, cable already attached) |
|---|---|---|
| ok, ID decoded | yes | PASS; record the ID (it is the camera adapter's, not yet known) |
| ok | `sig=se0` again | `arm pmu 20 180 --identify`, then `both`; then the same without the charger; then add `--power-handshake` |
| ok, but completed at `poll 0 ms` with a frame identical to the retained one | ? | suspect a stale event bit (assumes `0x0d` is read-to-clear, which the baseline read relies on), not a fresh reply; rerun the trial |
| `-110` (timeout) | — | check `idbus_frame` retained bytes and the `0x0d` events in the log; then `--identify=swap` once |
| `-74` (bad CRC) / `-71` (bad header) | — | the raw frame is in `identify.txt`; stop and analyse, no blind retries |
| `-16` (reset hold stuck) / `-19` (revision) / `-104` (detach) | — | stop and report; no further identify |

After the trial returns to device mode, reconnect the Mac cable and collect
the logs with the first trial's retrieval commands (`latest-identify.txt`
and every `<trial>/identify.txt` are in the tarball). Do not enable
`allow_power_handshake` from the command line.

## 2026-10-26 GetID trial (usb-host2-9977e33ee6, over Wi-Fi)

- Booting this kernel with the adapter attached and no Mac cable hangs at "A start job is running
  for Virtual Console Setup" (no limit). The boot waits on the USB gadget serial (`systemd.debug_shell=ttyGS0`).
  Plugging the Mac cable in let it continue. Boot with the cable, then swap.
- **Mac-cable baseline:** identify (no reorient) timed out after 100 ms, `0x0d` stayed 0. Before any
  write, the FIFO already held `74 00 02 00 00 00 00 66`, a leftover GetID TX with the Mac cable's
  known ID CRC `66` in byte 7.
- **Camera adapter:** first attempt timed out; after one reorient (`0x05=0x02`), **completion
  arrived** (`0x0d = 0x18`), but the FIFO read back `74 00 02 00 00 00 00 d6`, giving "bad header"
  (-71). Our TX bytes sit in 0..2 and byte 7 changed to `d6`, probably the adapter's ID CRC. The RX frame
  layout/offset in the FIFO (or a TX/RX overlay) is wrong in our read path.
- Host mode armed right after: still only root hubs, port `sig=se0`, `online=0`. Returned to
  device mode. Logs: `logs/usb-host2-20261026/`.

## Third trial: corrected CBTL1610 identify (usb-host3)

Prepared 2026-10-26, offline only; `hardware_tested=false`. This implements
the corrected [ds-lightning-host-idbus.md](ds-lightning-host-idbus.md) §5.4,
§9.2 and §9.6 after the GetID trial above. It is still a **manual** identify,
and the role switch is unchanged.

**Already in the trial evidence:** the first identify run's baseline read the
RX window **intact** before our TX write clobbered it:
`75 11 f0 00 00 00 00 d6` ([evidence.txt](../release/OMISSIONS.md),
the `258.452797` line). The header is right and `crc8(75 11 f0 00 00 00 00) = d6`
(the `11 f0` pair in the analysis table). So the camera adapter's ID is most
likely **`11 f0 00 00 00 00`**, decoded `USB=2 ACCx=0 DB=0 MB=0 UART=1 PWR=f HV=0
AI=0 AU=0 …`. Its `USB` value matches the Mac cable's, and `PWR=f` matches the
Haywire/EarPods class. It was a retained frame, not a fresh read, so this
trial should confirm it.

**Artifacts**
- Kernel branch **`a9/usb-host3`**, commit **`f585351c82`** (one commit on top
  of `a9/usb-host2` `9977e33ee6`; same two files). Patch:
  `patches/usbhost3/0001-power-supply-apple_tristar-use-the-CBTL1610-ID-bus-T.patch`.
  checkpatch `--strict`: 0 errors, 0 warnings, 0 checks. W=1 is clean.
- Entry **`usb-host3-f585351c82`**: `build/usb-host3/entry/`, payload
  `build/boot/m1n1-linux-usb-host3-f585351c82.bin`, manifest
  `build/usb-host3/manifest.json`. It has the same base as usb-host2 (tri52g/kexecfix
  cmdline and initramfs, m1n1 `psci=1`, exactly one
  `pcie_apple_h9p.ans_keep_power=1`), and debug info is off. The config and DTBs are
  byte-identical to usb-host2's. The cmdline is usb-host2's **minus
  `systemd.debug_shell=ttyGS0`** (see "No-cable boot hang" below). The
  packager asserts that this is the only difference.
- Build: `scripts/periph/build-usb-host3.sh` (it ends with `make clean`). Package only:
  `python3 -B -I scripts/periph/package-usb-host3.py`. All offline checks:
  `scripts/periph/check-usb-host3.sh`.
- Offline tests: `scripts/periph/test-tristar-idbus.sh` (now `TREE=usb-host3`
  by default) runs the kernel's own `apple_tristar_idbus.h` against a fake
  CBTL1610. The fake keeps the RX window 0x20..0x27 and the TX window
  0x5d..0x61 separate, and a write into 0x20 really overwrites the frame. The
  tests cover: the exact new register order, the TX window bytes at GO
  (`02 00 74 0c`), no GO after a readback mismatch, and RX accepted after a
  reorient with no second GetID (`75 11 f0 … d6`). A stale retained frame
  is refused and a GetID follows. There is a clobber regression test
  (the usb-host2 TX path fails it with 130 checks), and an invariant that
  nothing outside `0x05`, `0x0a` and `0x5d..0x61` is written. Fail-closed
  is tested at every I/O op, for identify, the power handshake and recover.
  The reset pulse always ends with the release. CRC vectors include
  `75 11 f0 00 00 00 00 → d6`. `python3 -B -I scripts/periph/test-usb-host.py`
  now covers the helper and iAP responder (43 tests). The harness was mutation-checked: no
  freshness gate, no readback compare, the TX written to 0x20, no release
  after an error, and an unconditional fault write are all caught.

**What `identify` does now** (`apple_tristar_idbus.h`, under the supplier mutex):
1. Check revision `0x8a`. Read the baseline `0x0c 0x0d 0x05 0x01 0x02 0x17
   0x0a` and the 8 bytes in the RX window at `0x20`, read only. A valid
   retained frame is logged as "may be stale" and never trusted on its own.
2. If `0x05` bit 3 is latched, clear only that bit and wait 20 ms.
3. GetID, exactly iOS's `sendIDBusCommand` order with nothing in between:
   `0x60 = 0x01` (prepare), block write `0x5d: 02 00 74 0c` (the request reversed,
   then the RX length byte `(7-1)<<1`), read the 4 bytes back and compare them, then
   `0x61 = 0x05` (GO). On a mismatch it stops with no GO: **`-121` (EREMOTEIO)**,
   no retry.
4. Poll `0x0d` every 10 ms for at most 100 ms. Completion is bit 3
   (hardware: `0x18`) or `(0x0d & 0x38) == 0x10`. Then read 8 bytes from `0x20`
   and `0x0c`, and require `0x75` plus CRC8. ID = bytes 1..6.
5. A timeout (poll expiry or the `0x40` event) with VBUS gets **one reorient**,
   `0x05 |= 0x02` (bit 0 kept; the bit self-clears). Then: 20 ms, at most 30 ms more of
   `0x0d` polling, and **the RX window is read before anything is written**.
   The frame is accepted if its header and CRC are right **and** it is fresh: a
   completion event after the reorient, or bytes that differ from the frame
   retained in step 1. An unchanged frame with no event may be the previous
   accessory's, and is refused as "same as the retained frame". Otherwise one
   more GetID follows, with the iOS 100 ms CmdID gap.
6. A transfer error (`0x20`), detach (`0x04`), bad header or bad CRC fails at once,
   and the raw frame is still published. A `0x0d` pin fault is acknowledged by
   echoing `0x0a`.

The worst case is 100 + 50 + 100 ms of waiting plus about 50 I2C transactions,
around 300 ms. The first I/O error refuses every later access in the run.
Only `0x05`, `0x0a` and `0x5d..0x61` are ever written. `0x20..0x27`,
`0x40/0x41`, `0x17`, `0x0e`, the mux registers, OTP/ESN, syscfg and NVRAM are not.
Writes use one plain I2C message (subaddress + data); reads use pointer + STOP,
then the read. `idbus_frame` now shows the source (`GetID command` or
`reorient-armed exchange`), `0x0c`, and the `after reorient` frame with its
verdict and `0x0d` events.

**Link recovery** (§9.6), `echo STEP > /sys/kernel/debug/apple-tristar/recover`
or `linux-a9-usb-host recover STEP`. The transcript goes to `recover_log`, and
helper copies to `<UTC>-recover-STEP/recover.txt` and `latest-recover.txt`. Every step
logs `0x0c` before and after:
- `reorient`: refused while `0x05` bit 3 is set. Reads `0x20` (before), sets
  `0x05 |= 0x02`, waits 20 ms, polls `0x0d` for at most 30 ms, then reads `0x20` and `0x0c`
  (read only; consumes the frame) and decodes a valid frame.
- `idbus_reset`: `0x05 |= 0x08`, 500 ms, then bit 3 cleared (with bit 0 kept; from
  `0x00` that is exactly `0x08` then `0x00`). Once the set may have landed, the
  release is sent even after an I/O error, with two retries. A bit 3 still set
  afterwards gives `-16`.
- `fault_ack`: reads `0x0a`, and writes that exact value back only if bits 0–3
  are set. Otherwise nothing is written.

**Power handshake** (still default-off, same gate): `0x72` goes through
`0x5f: 72 08`, GO `0x01`, and expects 6 bytes. Then `0x5d: echo flags 70 00`, GO
`0x05`, expecting 2 bytes. Only `(0x0d & 0x38) == 0x10` completes it.

**No-cable boot hang (fixed in this entry's cmdline).** The rootfs runs
systemd **262.1** (pacman db in `build/omarchy/rootfs.img`).
`systemd.debug_shell=ttyGS0` starts `debug-shell.service`. That unit has
`StandardInput=tty`, `TTYReset=yes` and `TERM=linux`, it is ordered
`After=systemd-vconsole-setup.service`, and the generator's drop-in sets `TTYPath=/dev/ttyGS0`.
In v262.1 `prepare_terminal()` (`src/core/exec-invoke.c`) runs before the shell is started.
With `TTYReset=yes` it takes `lock_dev_console()`, a BSD lock on `/dev/console`. It
resets the TTY, which turns `OPOST` on, and queries its size (non-blocking,
333 ms timeouts). Then, **still holding the lock**, `osc_context_open_service()`
writes its OSC sequence with a blocking `fputs`/`fflush`. On the ACM gadget,
`gs_write_room()` (`drivers/usb/gadget/function/u_serial.c`) returns **0 until
a USB host has configured the ACM function**. n_tty's `OPOST` output path therefore
sleeps, and without the Mac the write never completes and the lock is never
released. During sysinit, udev coldplug replays `vtcon` add events, and
`90-vconsole.rules` restarts `systemd-vconsole-setup.service`
(`Type=oneshot`, no start timeout, `Before=sysinit.target`). Its
`lock_dev_console()` then blocks forever, which is the "A start job is running for
Virtual Console Setup (no limit)" message. Plugging the Mac in configures ACM, the write
drains and the lock is released, so boot continues, exactly as observed. The initramfs is not
involved: it kills its own ttyGS0 getty/forwarder before `switch_root`.

The fix drops `systemd.debug_shell=ttyGS0` from this entry and **keeps**
`systemd.mask=maintainer@example.invalid`. `serial-getty@.service` also has
`TTYReset=yes` and a non-dumb TERM, so unmasking it would hold the same lock
during `getty.target` (that would not block boot, but would stall `getty@tty1` and later
vconsole restarts). The cost is that there is no root shell on ttyGS0 after `switch_root`. Rescue paths that
remain: the initramfs serial and telnet shells (before `switch_root`), SSH over
USB ECM with the Mac cable, and Wi-Fi SSH. Older entries keep the debug shell
and the hang, so boot them with the Mac cable attached. This conclusion comes from
source analysis; the optional check below confirms it.

### Owner test (usb-host3)

The orchestrator runs these on the Mac. Keep the recovery console ready
([warm-reboot.md](../boot/warm-reboot.md)).

```sh
cd .
export OMARCHY_HOST=172.16.42.1
S=scripts/omarchy/ssh.sh
"$S" 'findmnt -no SOURCE,FSTYPE /; uname -r; cat /proc/cmdline; sudo -n linux-a9-kexec status'
scripts/omarchy/install-usb-host.sh          # deploys the helper with `recover`; idempotent
# Warm kexec (needs the running entry staged, as in the first trial):
ENTRY=$(cat build/usb-host3/entry-name)
ENTRY="$ENTRY" scripts/boot/kexec-host.sh "$(cat build/usb-host3/payload-path)"
# ...or cold: scripts/boot.sh pongo, then
#   scripts/boot.sh linux "$(cat build/usb-host3/payload-path)"
"$S" 'tr " " "\n" </proc/cmdline | grep -E "linux_a9.entry|ttyGS"; cat /proc/sys/kernel/random/boot_id; findmnt -no SOURCE /; cat /sys/class/usb_role/lightning-role-switch/role; cat /sys/module/pcie_apple_h9p/parameters/ans_keep_power /sys/module/apple_tristar_power/parameters/allow_power_handshake; sudo -n ls /sys/kernel/debug/apple-tristar'
```

Expect `linux_a9.entry=usb-host3-f585351c82`, `systemd.mask=maintainer@example.invalid`
as the only ttyGS line (no `debug_shell`), a new boot ID, an NVMe root, role
`device`, `Y` then `N`, and twelve debugfs files (the usb-host2 ten plus
`recover` and `recover_log`). Then prove Wi-Fi and run the Mac-cable baseline:

```sh
scripts/periph/usb-host-wifi.sh
export OMARCHY_HOST=$(cat build/omarchy/wifi-host)
"$S" 'ip -br address; cat /proc/sys/kernel/random/boot_id; systemctl --user is-active linux-a9-udiskie.service'
# Baseline on the known Mac cable, over Wi-Fi, no reorient:
"$S" 'sudo -n linux-a9-usb-host identify --identify=noreorient'
```

Expect `digital_id: 10 0c 00 00 00 00`, a `frame: 75 10 0c 00 00 00 00 66 (header
and CRC ok), source GetID command`, and in the transcript `readback TX 0x5d matches` and
`write 0x61 = 0x05 (GO)`. That would be the first CPU-driven exchange to
complete. A `-110` here means the engine still ignores the CPU command; record
it and continue anyway, because the default mode's reorient path reads the
autonomous frame.

**The one command** (Wi-Fi SSH), the same 20 s / 180 s trial as before:

```sh
"$S" 'sudo -n linux-a9-usb-host arm none 20 180 --identify'
```

**Now tell the owner:** within 20 seconds, unplug the Mac cable and plug in
the Camera Adapter with the flash drive (and the charger on the adapter's
Lightning input, as before). Leave it attached.

Read over Wi-Fi from about t = 25 s:

```sh
"$S" 'sudo -n cat /var/log/linux-a9-usb-host/latest-identify.txt'
"$S" 'sudo -n journalctl -b -u linux-a9-usb-host-trial --no-pager | tail -30'
"$S" 'sudo -n linux-a9-usb-host status; sudo -n dmesg | grep -E "idbus|usb [0-9]|ehci|ohci|sd " | tail -80'
"$S" 'lsusb; lsusb -t; lsblk -o NAME,TRAN,SIZE,FSTYPE,LABEL,MOUNTPOINTS'
```

| identify result | enumeration | next |
|---|---|---|
| ok, `11 f0 00 00 00 00` (or another valid ID), either source | yes | PASS; record the ID and the source |
| ok | `sig=se0` again | `arm pmu 20 180 --identify`, then `both`; then the same without the charger; then add `--power-handshake` (each its own `arm`) |
| ok, source `reorient-armed exchange` | any | the ID is valid; the CPU GetID still went unanswered. Note it for the decode, not a failure |
| `-110` (timeout) | — | read the `after reorient` line in `idbus_frame` (frame, verdict, events); then `recover idbus_reset` once and repeat the trial |
| `-121` (readback mismatch) | — | the TX window did not hold our bytes: stop, keep the transcript, no retries |
| `-74` (bad CRC) / `-71` (bad header) | — | the raw frame is in `identify.txt`; stop and analyse |
| `-16` (reset hold stuck) / `-19` (revision) / `-104` (detach) | — | stop and report |

**If the Mac link stays dead afterwards** (Mac cable reconnected in both
orientations, no `/dev/cu.usbmodem*`, `usb0` without carrier), use Wi-Fi SSH
and try §9.6 in order, checking the Mac after each step:

```sh
"$S" 'sudo -n linux-a9-usb-host recover reorient'
"$S" 'sudo -n linux-a9-usb-host recover idbus_reset'
"$S" 'sudo -n linux-a9-usb-host recover fault_ack'
"$S" 'sudo -n ls -d /var/log/linux-a9-usb-host/*-recover-*'
```

**Optional no-cable boot check** (it validates the cmdline fix): with the
adapter attached instead of the Mac cable, warm-kexec this entry over Wi-Fi
the same way as the GetID trial (`OMARCHY_HOST=$(cat build/omarchy/wifi-host)`).
The boot should reach the desktop with no "Virtual Console Setup" start job,
and `kexec-host.sh` should see the new boot ID over Wi-Fi (the saved
NetworkManager profile should reconnect; not yet verified across a kexec). A hang shows up as a
`kexec-host.sh` timeout (`WAIT_SECONDS`, 240 s) with the start job on the
iPad screen. A timeout **without** the start job most likely means the
Wi-Fi address changed: plug the Mac cable in and rerun
`scripts/periph/usb-host-wifi.sh`. If it still stops at the start job, plug
the Mac cable in, note whether it continues, and report it: the cause would
then lie elsewhere.

After the trial returns to device mode, reconnect the Mac cable and collect
the logs with the first trial's retrieval commands. Do not enable
`allow_power_handshake` from the command line.

## Automatic iAP role-switch owner test

Revised after the usb-host4b EP0 trace. The exact HID descriptors, request
semantics and AppleSynopsysOTGDevice comparison are in
[ds-iap-roleswitch.md](ds-iap-roleswitch.md). Keep kernel branch
**`a9/usb-host4`**, commit **`09e836312a`**: the trace does not justify a
usb-host5 change. This kernel disables legacy `g_ffs` while retaining configfs
FunctionFS and has exactly one `pcie_apple_h9p.ans_keep_power=1`.

The listener leaves ACM/ECM in `g1/configs/c.1` and adds temporary
`ffs.iap` configuration 2. While VBUS is asserted in device mode it polls live
GetID every two seconds until the adapter is found, so a Mac-to-adapter
replacement is detected even if Tristar leaves VBUS online and DWC2 leaves the
Mac session configured. On a changed ID it
performs one clean UDC reconnect *before* the logged GetID and power handshake,
then leaves EP0 connected and stops polling. It waits indefinitely for both
attachment and request `0x51`; `180` below begins only if `0x51` switches to
host.

Run all commands below from the orchestrator Mac. Establish and use the
independent Wi-Fi path before changing the gadget:

```sh
cd .
scripts/periph/usb-host-wifi.sh
export OMARCHY_HOST=$(cat build/omarchy/wifi-host)
S=scripts/omarchy/ssh.sh
"$S" 'ip -br address; findmnt -no SOURCE /; cat /sys/class/usb_role/lightning-role-switch/role'

# Installs both userspace helpers, including live-GetID cable detection and the
# pre-identify reconnect. It does not switch roles and is idempotent.
scripts/omarchy/install-usb-host.sh

# Prove usb-host4, clean up any earlier listener, and enable ring-buffer-only
# DWC2/composite debug. No rebuild or usb-host5 entry is needed.
"$S" 'tr " " "\n" </proc/cmdline | grep -E "linux_a9.entry|ans_keep_power|g_ffs"; test ! -e /sys/module/g_ffs; grep -E "CONFIG_USB_(CONFIGFS_F_FS|FUNCTIONFS)" /boot/config-$(uname -r) 2>/dev/null || true; cat /sys/class/usb_role/lightning-role-switch/role; findmnt -no SOURCE /'
"$S" 'sudo -n linux-a9-usb-host device'
"$S" 'grep -F "Live GetID changed" /usr/local/sbin/linux-a9-usb-host'
"$S" "sudo -n sh -c 'echo \"file drivers/usb/dwc2/gadget.c +p\" > /sys/kernel/debug/dynamic_debug/control; echo \"file drivers/usb/gadget/composite.c +p\" > /sys/kernel/debug/dynamic_debug/control'"
"$S" 'sudo -n logger -k -t linux-a9-usb-host "NEXT iAP discriminator begins"'

# Returns immediately after starting the service. 180 seconds is only the
# post-0x51 host safety window; there is no plug-in or owner deadline.
"$S" 'sudo -n linux-a9-usb-host listen none 180 --identify --power-handshake'
"$S" 'sudo -n linux-a9-usb-host status; sudo -n systemctl --no-pager status linux-a9-usb-host-listen.service'
```

Expect entry `usb-host4-09e836312a`, no `/sys/module/g_ffs`, `role=device`,
`listener=active`, `udc=20c100000.usbdev`, `iap_config=True`, and no plug-in
deadline. The listener initially reconnects configuration 1 cleanly and logs
the Mac ID; ECM/ACM must enumerate again before the cable is removed.

The owner may now unplug the Mac and attach the Camera Adapter plus flash drive
**whenever they choose**. There is no follow-up command and no timing window.
Even if VBUS never falls, within one polling interval the journal should show:

```text
Live GetID changed 10 0c 00 00 00 00 -> 11 f0 00 00 00 00
Attachment event (GetID cable change): clean gadget reconnect; running default GetID
IDENTIFY ok: ID 11 f0 00 00 00 00
POWER HANDSHAKE ok
Power handshake complete; preserving the gadget connection for post-handshake enumeration.
```

There must be one reconnect before the logged adapter GetID and none after the
`0x71` reply. `listen-identify` remains an operator diagnostic, but it is not
part of this event-driven test.

After attachment, read the live evidence over Wi-Fi:

```sh
"$S" 'sudo -n tail -100 /var/log/linux-a9-usb-host/latest-iap-control.txt'
"$S" 'sudo -n cat /var/log/linux-a9-usb-host/latest-identify.txt; sudo -n cat /var/log/linux-a9-usb-host/latest-power-handshake.txt'
"$S" 'sudo -n journalctl -b -u linux-a9-usb-host-listen.service --no-pager | tail -100'
"$S" 'sudo -n linux-a9-usb-host status; lsusb; lsusb -t; lsblk -o NAME,TRAN,SIZE,FSTYPE,LABEL,MOUNTPOINTS'
"$S" 'sudo -n dmesg | grep -E "dwc2|idbus|ehci|ohci|usb [0-9]|sd " | tail -120'
"$S" 'sudo -n cat /sys/kernel/debug/apple-usb-role/regs'
```

Pass criteria:

1. The listener detects the ID change without a VBUS edge, performs exactly one
   clean reconnect before GetID, reports `11 f0 00 00 00 00`, completes the
   power handshake, and does not reconnect after `0x71`.
2. DWC2 logs a full-speed reset and then `new address`; `control.log` contains
   the descriptor reads and
   `bmRequestType=0x40 bRequest=0x51 ... wLength=0`, followed by
   `ROLE_SWITCH accepted and ACKed; delay=50ms`.
3. The helper changes to `role=host`; EHCI/OHCI report an attached device, not
   `not attached`/`sig=se0`.
4. The flash drive appears under `lsusb` and `lsblk` (and mounts through the
   existing desktop automounter).

Failure is still useful and is split at the protocol boundary:

| Observation | Meaning / next action |
|---|---|
| no `Live GetID changed` while the adapter answers manual GetID | polling regression; preserve the journal and do not reinterpret it as an adapter failure |
| reconnect occurs after successful `0x71` | wrong userspace helper installed; stop before interpreting USB results |
| full-speed reset, EP0 OUT armed, then `EarlySusp`, with no setup/address | corrected ordering did not change the address-zero failure; next isolate Linux's reset-time EP0 rearm from adapter policy |
| real `timeout flushing fifos (GRSTCTL=...)` at the adapter reset | DWC2 core/FIFO failure; unlike `timeout at 99`, this supports a kernel fix |
| no reset after the clean reconnect or handshake | preserve Tristar, role and DWC2 registers; investigate adapter-side readiness/power policy next |
| power handshake fails | preserve its raw `0x72/0x73/0x70` transcript; the pre-identify reconnect remains the only reconnect |
| `new address`, but no FunctionFS requests | device descriptor succeeded; adapter rejected or did not select configuration 2 |
| HID/report reads or `SET_REPORT`, but no `0x51` | descriptor recognition or pre-switch iAP behavior is incomplete; preserve payload bytes |
| `0x51` stalled | record its exact value/index; the inferred zero masks need updating |
| `0x51` accepted, host port still `not attached` | HID emulation succeeded; continue the separate VBUS/routing experiment |
| drive enumerates | PASS; run `linux-a9-usb-host confirm` only if it must remain mounted past 180 s |

Do not repeat the direct-host or gate matrix for this discriminator. Do not
manually toggle `soft_connect`: the automatic pre-identify reconnect is the
variable under test.

At any point, `device` is the clean stop. It stops the listener even if the
unit is inactive, mount-safely returns from host mode, removes `c.2/ffs.iap`,
discovers the sole UDC if the `UDC` attribute is empty, and rebinds c.1:

```sh
"$S" 'sudo -n linux-a9-usb-host device; sudo -n linux-a9-usb-host status'
"$S" 'test ! -e /sys/kernel/config/usb_gadget/g1/configs/c.2; cat /sys/kernel/config/usb_gadget/g1/UDC'
```

Do not write syscfg, NVRAM, or boot-firmware storage. None of these commands
does so.
