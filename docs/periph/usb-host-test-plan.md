# Lightning USB host mode: orchestrator test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Round 1 of USB host mode (keyboards, mice, flash drives, hubs)
through the Lightning port, from [ds-usb-host.md](ds-usb-host.md). The
implementation agent did no device access and no workspace commit.

| | |
|---|---|
| Kernel branch | `a9/usbhost` at `07377c65b6` (from `hoolock` 6831bc701a), 7 commits. `4a908dfd0d` (in nextint `7e753d3036`) failed to probe with `-EBUSY` on `0x20c900000`; fixed by the last 3 commits |
| DTB change | **yes**, j71t only (`s8003-j71t.dts` includes `s8003-j71t-usbhost.dtsi`). Warm kexec is fine once the running cold-boot entry is staged |
| Config fragment | `patches/usbhost/usbhost.config` (not yet in `scripts/integrate.sh` `FRAGMENTS`) |
| Trial merges | clean into `a9/nextint`, `a9/periph`, `a9/idle`, `a9/touch`, `a9/aop`, `a9/boot`, `a9/display` |
| Offline checks | standalone full build (`build/kernel-usbhost/`, Image.gz + 104 DTBs). W=1 clean on the driver and PHY, both standalone and merged into nextint (with nextint's config + fragment). checkpatch `--strict`: 0 errors, only the Co-Authored-By/MAINTAINERS notes. `dt_binding_check` passes. j71t `dtbs_check` adds no new warning, standalone or merged; a negative test confirms the schema is applied. The only dtc W=1 note is the existing `simple_bus_reg` on `bus@20c000000` |

## What the branch does

1. **`phy: apple: ausb`**: the compiled-out `set_mode` now works. A role
   change writes the mode's `cfg0/cfg1`, zeroes the trim word (`+0x1c`)
   and pulses the PHY reset (20 µs, then 1 ms), as iOS's
   `enableHostMode` does in both directions. It no longer writes
   `AUSB_SIG` bits 12–14 (iOS doesn't). Power-off resets the PHY to
   device mode, so DWC2's power-on still gets the device tunables. Device
   mode is unchanged.
2. **`usb: roles: apple-s8000-usb-role`**: a `usb_role` provider named
   `lightning`. Device mode is the resting role. Host mode:
   - unbinds the gadget driver (configfs `g1`); DWC2 (dr_mode peripheral)
     then drops its pull-up and its PHY reference;
   - turns on the opt-in accessory power gates (`vbus_gates`);
   - checks that the EHCI0/OHCI0 DMA remap words are `0x108`. The
     usb-complex driver (`drivers/bus/apple-usbcomplex.c`) owns and programs
     them at probe; the switch only reads them through a shared mapping and
     repairs a lost value;
   - powers the PHY, sets `USB_CTL+0` bit 0, then switches the PHY to the
     host tunables (`0x37373b03`/`0x00020c04`) with a reset pulse;
   - creates the EHCI (`20c200000.usb`) and then its OHCI companion
     (`20c300000.usb`) from the switch's child DT nodes.

   Device mode undoes these steps in reverse order and rebinds the same
   gadget. An unconfirmed host role goes back to device mode after
   `revert_timeout` seconds (default 60). The revert runs in the kernel and
   needs no userspace.
3. **DT**: the switch node `usb-role-switch@20e0d8000` sits inside the
   usb-complex. It has USB_CTL (`0x20e0d8000`, ADT `otgphyctrl` reg[1],
   the only range it claims) and the glue words (`0x20c000000`, mapped
   without claiming them), the EHCI/OHCI children (AIC 215/216, `ps_usb2host0`/`ps_usb2host0_ohci`),
   the D2365 PMIC and the Tristar at `i2c0 0x1a`.

**The suspected PHY address bug doesn't exist on A9.** `s800-0-3.dtsi`
already puts `ausbphy0` at `0x20c000030`, as the ADT does. The
`0x20c000020` address is in the A7 `s5l8960x.dtsi`, which J71t doesn't
use. In the ADT, `0x20c000020` (16 bytes) is the DWC2's second reg range
(device glue), `0x20c000000` and `0x20c000008` are the EHCI and OHCI
glue words, and the PHY starts at `0x30`.

### Interfaces

| Path | Meaning |
|---|---|
| `/sys/class/usb_role/lightning-role-switch/role` | `device` / `host` (rw). `none` counts as `device`. Writing `host` again while unconfirmed restarts the countdown |
| `/sys/bus/platform/devices/20e0d8000.usb-role-switch/revert_timeout` | Seconds before an unconfirmed host role reverts. `0` means never. Read at host entry. Default from `apple_s8000_usb_role.revert_timeout=60` |
| `…/revert_pending` | Seconds left before the revert (0 if none) |
| `…/confirm` | `echo 1` keeps host mode (cancels the revert) |
| `…/vbus_gates` | `none` (default) / `pmu` / `tristar` / `both`, applied at the next host entry and removed at exit |
| `/sys/module/apple_s8000_usb_role/parameters/clear_device_enable` | `Y` also clears `USB_CTL+4` bit 1 in host mode. iOS's PHY code doesn't, but its terminated DWC2 nub may. Default `N` |
| `/sys/kernel/debug/apple-usb-role/regs` | One-shot dump: role, PHY refcounts, `USB_CTL`, remap, PHY regs, DWC2 (while bound), EHCI/OHCI and their glue (only while bound), Tristar `0f/01/05/0c`, PMU LDO5 `0310/2600/2603` |
| `…/pmu_ldo5`, `…/pmu_ldo5_switch`, `…/tristar_vbus_sink` | Raw 0/1 knobs for measurements. They act immediately in any role and bypass `vbus_gates` bookkeeping. Turn them off again yourself |

Accessory power details. D2365 LDO5 is the iOS PMU LDO table row 5:
enable `0x0310` bit 0, voltage `0x2600` (2.5 V + 25 mV/step, never
written), switch `0x2603` (iOS writes the whole byte, 1/0). The ADT
`dock` uses `acc_pwr = pmuL(0x5)` and `acc_sw_en = pmuL(0x80000005)`.
`acc_sleep_pwr = pmuL(0x220)` is bits 1–3 of `0x0310` and is never
touched. The Tristar gate is register `0x05` bit 0 (`setVBUSSink`, 18 ms
settle), written only when `0x0f == 0x8a`. The `pmu` gate enables the
rail before the switch; that order is inferred. A 2.5–3.3 V LDO is
accessory power, **not 5 V VBUS**.

### Known limits (round 1)

- **No automatic switch yet.** The adapter's vendor request `0x51` needs
  an `IapOverUsbHid` interface that our gadget doesn't offer. Step 2
  captures what the adapter actually sends, to decide round 2.
- **Accessory identification (ID bus) isn't implemented.** Under Linux
  nothing may power or route the Camera Adapter. The `pmu` gate and the
  Tristar route reads in steps 2 and 4 answer this.
- **Charger interaction.** In host mode the gadget is unbound, so the
  a9/periph Tristar driver sees "not enumerated". If VBUS is present
  (the Mac cable in step 3, or a powered USB3 adapter), about 1 s later it
  may run its one-time brick check, which reroutes D+/D- to the ADC for
  about 2×5 ms. It also reports a 500 mA source. A small glue patch is
  suggested for a9/periph: in `apple_tristar_update()`, report offline and
  skip `apple_tristar_classify()` while
  `usb_role_switch_get_role(fwnode_usb_role_switch_get(...)) == USB_ROLE_HOST`
  (look up the switch node by compatible `apple,s8000-usb-role-switch`).
  Step 7 checks whether this is needed.
- **a9/idle PHY runtime PM.** `phy_apple_ausb.runtime_pm=1` (default
  off) swaps in `ausb_pm_phy_ops`, which has no `.set_mode`, so host
  tunables wouldn't be written. Keep it off for these tests. Adding
  `.set_mode = ausb_phy_usb2_set_mode` to those ops is a one-line
  follow-up in the nextint merge.
- Suspend is refused while in host mode ("return to device mode before
  suspending").
- A configfs `UDC` write while in host mode would rebind DWC2 under the
  host. Don't do that; use the role file.

## Integration (host only)

1. Merge `a9/usbhost` into the next integration tree. All trial merges
   were clean.
2. Add `patches/usbhost/usbhost.config` to the build. It enables the
   switch and also USB mass storage (`SCSI`, `BLK_DEV_SD`, `USB_STORAGE`,
   `USB_UAS`, `EXFAT_FS`), which today's nextint config lacks, so flash
   drives would not show up otherwise. EHCI/OHCI platform, HID, VFAT and
   NLS are already on.
3. Build and package as usual. Before kexec, stage the running cold-boot
   entry (changed DTB). Boot with **`omarchy.root=nvme`**. With an NBD root
   over the USB link, root I/O stalls (doesn't fail) for as long as host
   mode lasts. Make sure `build/omarchy/wifi-host` is current:
   `scripts/omarchy/ssh.sh` falls back to Wi-Fi when the USB address
   doesn't answer, and from step 2 on Wi-Fi is the only link.
4. Leave `phy_apple_ausb.runtime_pm` unset.

Below, `S='scripts/omarchy/ssh.sh'` and
`R=/sys/bus/platform/devices/20e0d8000.usb-role-switch`. Device writes
use `sudo -n tee`. From step 2 on, `export
OMARCHY_HOST=$(cat build/omarchy/wifi-host)`, so that no ssh session
rides on the USB link that the step is about to drop.

## Step 0: boot, device-mode regression (Mac cable)

```sh
$S 'sudo -n dmesg | grep -E "apple-s8000-usb-role|bound driver|20c[23]00000"'
$S 'cat /sys/class/usb_role/lightning-role-switch/role; ls /sys/bus/platform/devices | grep -c "20c[23]00000.usb"; findmnt -no SOURCE /'
ping -c3 172.16.42.1
```

PASS:
- `apple-s8000-usb-role 20e0d8000.usb-role-switch: device role; host on request (revert after 60 s)`,
  `/sys/class/usb_role/lightning-role-switch` exists, and no
  `sync_state() pending due to 20e0d8000.usb-role-switch` line (fw_devlink
  hangs the child nodes' PMGR domains on the switch, so a failed switch
  probe leaves `power-controller@80260/80268` pending)
- `dwc2 20c100000.usbdev: bound driver configfs-gadget.g1`
- role `device`, `0` host controllers, root on `/dev/nvme…`
- ECM pings answer.

FAIL: anything missing, or ECM down. Then stop and capture `dmesg`.

## Step 1: read-only baseline (Mac cable, device mode)

This is the first access to `USB_CTL` (`0x20e0d8000`, inside the PMGR
1 MB window per the ADT; probe doesn't touch it). If the read hangs or
SErrors, the bank needs a clock. Report that and stop.

```sh
$S 'sudo -n cat /sys/kernel/debug/apple-usb-role/regs'
```

Expected ([I] = inferred):

| Line | Expected |
|---|---|
| `role` | `device`, accessory power `none` |
| `usb_ctl` | `+00` bit 0 = 0; `+04` bit 1 = 1 [I] |
| `remap` | `+1c +3c +5c +7c +9c` = `00000108` (usb-complex driver) |
| `phy` | `+08=37377bc3 +0c=00020c04`; `+04` (SIG) bit 8 set with the cable (m1n1 notes `0xd103`) |
| `dwc2` | `ghwcfg2 & 7 == 4` (device-only, matches the debugfs hw_params result) |
| `device glue` | record |
| `tristar` | `0f=8a`, `05` bit 0 = ? (record), `0c & 3 == 3` with the Mac |
| `pmu ldo5` | record `0310 / 2600 / 2603` |

## Step 2: Camera Adapter in device mode (read-only, Wi-Fi)

Unplug the Mac cable and plug in the Lightning-to-USB Camera Adapter
(USB2 or USB3) with **nothing attached** to it. Use Wi-Fi from here on.

```sh
$S 'echo "func dwc2_hsotg_process_control +p" | sudo -n tee /sys/kernel/debug/dynamic_debug/control'
# unplug / replug the adapter, wait 5 s
$S 'sudo -n cat /sys/kernel/debug/apple-usb-role/regs; sudo -n dmesg | tail -40'
$S 'echo "func dwc2_hsotg_process_control -p" | sudo -n tee /sys/kernel/debug/dynamic_debug/control'
```

Record:
- `phy +04` (SIG): m1n1 notes `0x1c003` with a USB-A adapter (bit 16 set,
  bit 12 clear, bit 8 clear). If we see that, SIG is a usable
  adapter-present signal for round 2.
- Tristar `01` (data route nibbles; `1` = USB) and `0c`. Is the USB lane
  routed with the adapter in?
- PMU LDO5: is accessory power already on? If `0310` bit 0 = 0, the
  adapter is unpowered under Linux.
- dmesg `ctrl Type=.. Req=..` lines. If the adapter enumerates the iPad,
  they show its requests. Look for `Type=40, Req=51` (role-switch
  request) and `Type=c0, Req=53`. No lines means the adapter doesn't talk
  to the gadget (probably unpowered).

Then repeat once with a keyboard plugged into the adapter.

## Step 3: manual switch and auto-revert (Mac cable, no adapter)

This checks the switch mechanics with no accessory question in the way.
ECM drops, which is expected. Watch from Wi-Fi.

```sh
$S 'echo host | sudo -n tee /sys/class/usb_role/lightning-role-switch/role'
$S "cat $R/revert_pending; sudo -n cat /sys/kernel/debug/apple-usb-role/regs; sudo -n dmesg | tail -30"
sleep 70
$S 'cat /sys/class/usb_role/lightning-role-switch/role; sudo -n dmesg | tail -15'
ping -c3 172.16.42.1
```

PASS, in this order in dmesg:
- `unbinding gadget driver configfs-gadget.g1`
- `ehci-platform 20c200000.usb: new USB bus registered, assigned bus number 1`,
  `USB 2.0 started, EHCI 1.x0`, `hub 1-0:1.0: 1 port detected`
- `ohci-platform 20c300000.usb: … assigned bus number 2`, `hub 2-0:1.0: 1 port detected`
- `host mode (USB_CTL 0x…1/…, accessory power: none)`
- `returning to device mode in 60 s unless confirmed`
- `regs` during host mode shows `usb_ctl +00` bit 0 = 1,
  `phy +08=37373b03`, `ehci: … cf=00000001`, `phy: init 1 power 1, held by switch: 1`.
- About 60 s later: `host mode was not confirmed, returning to device mode`,
  `gadget driver configfs-gadget.g1 bound again`, `dwc2 … bound driver configfs-gadget.g1`.
- Role `device`; the Mac's ECM pings answer again within about 10 s
  (DHCP from the iPad; on the Mac, if needed, bounce the interface:
  `sudo ifconfig <enX> down; sudo ifconfig <enX> up`).

FAIL: an oops, no EHCI bind (`no driver bound`), or ECM not back after
the revert. Capture `dmesg` and `regs`, then `echo device | …/role`.

Also check the explicit return:
`echo host …; sleep 5; echo device …` gives the same PASS lines without
the timeout message.

## Step 4: enumeration through the Camera Adapter (Wi-Fi)

Plug in the adapter plus a **keyboard**, a **flash drive** (FAT/exFAT),
and later a hub. Give yourself time:

```sh
$S "echo 300 | sudo -n tee $R/revert_timeout"
```

Try the gates in this order and stop at the first that enumerates. Return
to device mode between attempts.

```sh
# a) no accessory power
$S "echo none | sudo -n tee $R/vbus_gates; echo host | sudo -n tee /sys/class/usb_role/lightning-role-switch/role"
sleep 8
$S 'sudo -n cat /sys/kernel/debug/apple-usb-role/regs; sudo -n dmesg | tail -30; lsusb -t 2>/dev/null || sudo -n cat /sys/kernel/debug/usb/devices'
$S 'echo device | sudo -n tee /sys/class/usb_role/lightning-role-switch/role'
# b) PMU accessory LDO:   echo pmu  > vbus_gates, repeat
# c) PMU + Tristar sink:  echo both > vbus_gates, repeat
# d) as c) plus:  echo Y | sudo -n tee /sys/module/apple_s8000_usb_role/parameters/clear_device_enable
```

Read `ehci: portsc1` in `regs`: bit 0 (connect) and bits 11:10 (line
state) show whether anything is electrically present. `ohci: port1` bit 0
shows the same for low/full-speed devices after the EHCI hands the port
over (`portsc1` bit 13 = owner).

PASS:
- Keyboard (low/full speed, on the OHCI): `usb 2-1: new low-speed USB device number 2 using ohci-platform`
  (or `full-speed`), then `input: … as /devices/platform/soc/20c900000.bus/20e0d8000.usb-role-switch/20c300000.usb/usb2/2-1/…`.
  Typing works:
  `$S 'sudo -n timeout 10 cat /dev/input/by-id/*-event-kbd | od -An -tx1 | head'`
  (press keys during the 10 s), or keys appear on the iPad's screen.
- Flash drive (high speed, on the EHCI): `usb 1-1: new high-speed USB device number 2 using ehci-platform`,
  `usb-storage 1-1:1.0: USB Mass Storage device detected`, `sd 0:0:0:0: [sda] … Attached SCSI removable disk`.
  Then read it without writing:
  `$S 'lsblk; sudo -n mount -o ro /dev/sda1 /mnt && ls /mnt | head && sudo -n umount /mnt'`.
- Hub: `hub 1-1:1.0: USB hub found`, then devices behind it.
- `echo 1 | sudo -n tee $R/confirm` gives `host mode confirmed` and
  `revert_pending` reads `0`.

Expect the adapter to report "too much power" for some devices: iOS
grants 100 mA per port (`AAPL,power-supply 0x32`).

Return: `echo device …/role`, unplug the adapter, plug in the Mac cable,
and ECM comes back (step 3 PASS lines).

## Step 5: plain Lightning-to-USB-C cable and a powered hub (experiment)

**This can fail for hardware reasons that no driver can fix.**
- A Lightning-to-USB-C cable presents Rd on its C end because it expects
  a host or charger there. A hub's upstream port also presents Rd, so
  neither side sources CC or VBUS and the hub never attaches.
- The iPad can't drive 5 V VBUS out of the Lightning power pin. LDO5 is a
  low-voltage accessory rail, and the Tristar bit gates VBUS *into* the
  iPad.
- So the only plausible case is a hub that **sources** power on its
  upstream port (PD pass-through "charging" hubs). VBUS is then present
  and the hub may enable its upstream pull-up. A bus-powered or plain
  self-powered hub is expected to fail.

```sh
$S "echo both | sudo -n tee $R/vbus_gates; echo 300 | sudo -n tee $R/revert_timeout"
$S 'echo host | sudo -n tee /sys/class/usb_role/lightning-role-switch/role'
sleep 8
$S 'sudo -n cat /sys/kernel/debug/apple-usb-role/regs; sudo -n dmesg | tail -30; cat /sys/class/power_supply/usb/online 2>/dev/null'
$S 'echo device | sudo -n tee /sys/class/usb_role/lightning-role-switch/role'
```

PASS: `hub 1-1:1.0: USB hub found`. Otherwise record `tristar 0c`, SIG
and `portsc1`. A clean FAIL with no connect bit is an acceptable result.

## Step 6: return path and stress (Mac cable)

```sh
for i in $(seq 1 10); do
  $S 'echo host | sudo -n tee /sys/class/usb_role/lightning-role-switch/role' >/dev/null
  sleep 5
  $S 'echo device | sudo -n tee /sys/class/usb_role/lightning-role-switch/role' >/dev/null
  sleep 12
  ping -c2 -t3 172.16.42.1 >/dev/null && echo "cycle $i ok" || echo "cycle $i ECM DOWN"
done
$S 'sudo -n cat /sys/kernel/debug/apple-usb-role/regs | head -3; ls /sys/bus/platform/devices | grep -c "20c[23]00000.usb"'
```

PASS: 10× `ok`; then `role: device`, accessory power `none`,
`phy: … held by switch: 0`, `0` host controllers, and no `WARNING`/`BUG`
in dmesg. Also run 5 host-mode plug/unplug cycles of the keyboard
(step 4 setup).

## Step 7: charging and Tristar interaction

During steps 3 and 4 (and a powered USB3 adapter, if available):

```sh
$S 'cat /sys/class/power_supply/usb/{online,current_max,usb_type} 2>/dev/null; sudo -n dmesg | grep -E "brick|Tristar|tristar" | tail'
```

Record whether `Apple brick signature …` or `brick detection unavailable`
appears while the role is `host`. If it does with a device attached, apply
the a9/periph glue described under *Known limits*.

## Step 8 (optional): shutdown and kexec from host mode

Enter host mode, confirm, then warm-kexec into the same entry. PASS: the
new kernel's gadget enumerates on the Mac (shutdown clears `USB_CTL` bit 0
and restores the PHY's device tunables), and `regs` in the new kernel shows
`usb_ctl +00` bit 0 = 0. Also check `systemctl suspend` in host mode: it
is refused with `return to device mode before suspending`.

## Results to report

One table row per step: PASS/FAIL, the `regs` dump(s), and the dmesg
excerpt. Step 2's SIG, Tristar `01/0c`, LDO5 and `ctrl Type=` lines, plus
step 4's working gate combination, decide round 2: automatic switching
(vendor request `0x51` or a SIG/Tristar detect) and whether the gates can
default on.
