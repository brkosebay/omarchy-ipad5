# Omarchy over NBD: first device boot (orchestrator steps)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

**Success criteria, in order** (stop at the first one that fails and send the logs):

1. **Boot 1 pass:** systemd reaches `multi-user.target` with root on `/dev/nbd0`, and `ssh linux@172.16.42.1` works. `systemctl is-system-running` should say `running` (the round-1 `omarchy-provision-hardware` failure is fixed); `degraded` with a list of failed units is a soft pass.
2. **Input:** the Keychron K2 pairs with `ipad-pair-keyboard`, shows up as an input device, and reconnects after `bluetooth` restarts.
3. **Desktop:** Hyprland with Omarchy's bar on the iPad screen (simpledrm + llvmpipe, slow), driven from the K2.

## 0. Preconditions (offline, any time)

1. **Kernel = integ with the Omarchy fragment.** `configs/omarchy.fragment` is in `scripts/integrate.sh`'s list, but `build/kernel-integ` (13:33) predates that, so it has none of the fragment's options. Rebuild, then check:
   ```sh
   scripts/integrate.sh
   scripts/omarchy/check-fragment.sh build/kernel-integ/config    # "80 options checked, 0 mismatched"
   ```
> Private graphics-dependent excerpt omitted from this source snapshot.

## 1. Start the root server (terminal 1, leave it running)

```sh
scripts/omarchy/serve-root.sh
```

It prints `waiting up to 900s for 172.16.42.2 ...`, binds only 172.16.42.2:10809 (export `omarchy`), and restarts itself when the USB link has been gone for 20 s, so it survives iPad reboots. Stop it with Ctrl-C, **only once the iPad is powered off or sitting in the rescue shell**.

## 2. Payload

```sh
TREE=integ scripts/mkpayload.sh build/omarchy/initramfs-nbd.gz hid_apple.fnmode=2
```

Resulting bootargs: `earlycon console=tty0 loglevel=7 hid_apple.fnmode=2`.
- `hid_apple.fnmode=2` makes F1–F12 the default on the K2 in Mac mode (fn + F-key gives media keys). `hid_apple` is built in, so Omarchy's `/etc/modprobe.d/hid_apple.conf` (same setting, written by its F-key step) can never reach it. Don't try to fix it through modprobe.d. As a fallback the image also writes `2` to `/sys/module/hid_apple/parameters/fnmode` at boot (tmpfiles; the parameter is runtime-writable and read on every key event, **verified** in `drivers/hid/hid-apple.c`).
- The initramfs needs no arguments. Optional: `omarchy.wait=900` (default NBD wait 600 s), `omarchy.rescue` (stop in the rescue shell), `fbcon=rotate:1` (landscape boot console; the panel is portrait).

## 3. Boot

`scripts/boot.sh pongo` (the human does DFU), then `scripts/boot.sh linux`.

## 4. What to expect

On the iPad screen (fbcon, portrait, kernel log at loglevel 7; initramfs lines carry `[omarchy-rd]`):
1. `USB network: usb0 172.16.42.1/24, DHCP for the Mac: 172.16.42.2`
2. `Charger: requested 2.4 A from the USB host` (or `no ... yet`, then the rootfs asks again)
3. `Waiting up to 600s for NBD 172.16.42.2:10809 export 'omarchy'`
4. `NBD connected: /dev/nbd0 24 GiB`, then `Switching to /usr/lib/systemd/systemd on /dev/nbd0`
5. systemd's boot. The **first boot** only also runs `omarchy-provision-hardware` (pacman keyring with gpg key generation, about 4 s of CPU in QEMU, plausibly a minute on the A9, inferred).
6. A text login prompt on tty1 (`omarchy-ipad login:`). Nobody can log in there (no passwords); that's expected.

On the Mac:
- Terminal 1: `172.16.42.2 is up on enX; serving ...`.
- `/dev/cu.usbmodem*` appears as soon as the gadget binds: a root shell in the initramfs, then a root autologin console (`serial-getty@ttyGS0`) after `switch_root`.

## 5. Checks over SSH (criterion 1)

```sh
ssh linux@172.16.42.1                      # key auth, ~/.ssh/id_ed25519
```

Run this and save the output to `docs/test-logs/2026-10-22-omarchy-boot1.txt`, and the journal next to it:

```sh
ssh linux@172.16.42.1 '
  systemctl is-system-running; systemctl --failed --no-legend
  findmnt -no SOURCE,FSTYPE,OPTIONS /        # /dev/nbd0 ext4 rw,noatime
  uname -r; getconf PAGESIZE                 # 7.3.0-rc1-g<integ>, 16384
  omarchy-hw-platform                        # aarch64  (stock Omarchy would say aarch64-apple)
  ls /var/lib/omarchy/image/                 # lock target.booted (nothing else once provisioning finished)
  systemctl status omarchy-provision-hardware --no-pager | head -4
  sudo journalctl -b -u omarchy-provision-hardware -o cat | grep -E "First boot|nothing to rebuild|complete|Error"
  ip -br addr show usb0; systemctl is-active sshd linux-a9-usb0-dhcp serial-getty@ttyGS0
  sudo ufw status | head -12
  cat /sys/class/graphics/fb0/virtual_size   # expected 1536,2048 (portrait); not a gate, see step 7
  systemctl status linux-a9-charger-limit --no-pager | head -3
  cat /sys/class/power_supply/charger/input_current_limit /sys/class/power_supply/*/status 2>/dev/null
  systemctl is-active bluetooth; sudo btmgmt info | grep -E "addr|current settings"
  sudo ls -l /usr/lib/firmware/brcm/BCM.apple,j71t.hcd
  sudo dmesg | grep -iE "bluetooth|hci0|bcm" | tail -15
  cat /sys/module/hid_apple/parameters/fnmode /sys/module/zswap/parameters/enabled   # 2, N
  free -m; swapon --show
  systemd-analyze; systemd-analyze blame | head -15
  sudo dmesg | grep -iE "nbd|stuck request|firmware|error" | tail -30'
ssh linux@172.16.42.1 'sudo journalctl -b --no-pager' > docs/test-logs/2026-10-22-omarchy-boot1-journal.txt
```

Pass: `running` (or `degraded`, with the failed units listed), root on `/dev/nbd0`, the SSH session works. Report `systemd-analyze`, anything `--failed`, and whether `btmgmt info` shows `powered ... br/edr le` (bluetoothd turns on both in its default dual mode, **inferred** from BlueZ; `ipad-pair-keyboard` turns them on itself if not).

## 6. Pair the Bluetooth keyboard (criterion 2)

1. On the K2: side switch on Bluetooth, then **hold fn+1 for about 4 s** until the key light blinks fast (pairing mode, slot 1).
2. On the Mac:
   ```sh
   ssh -t linux@172.16.42.1 ipad-pair-keyboard
   ```
   It powers the controller (turning on BR/EDR and LE if needed), scans for up to 40 s, picks the first unpaired keyboard (class "keyboard" / icon `input-keyboard`), and pairs with agent `KeyboardDisplay`. The K2 uses Secure Simple Pairing passkey entry, so it prints:
   ```
   ==================================================
     On the keyboard, type:   123 456
     then press Enter.
   ==================================================
   ```
   **Type the 6 digits on the K2, then Enter.** The helper then trusts and connects the keyboard, and prints its state and the input device (`Keychron K2 -> sysrq kbd leds event3 ...`). The bluetoothctl transcript goes to `/run/ipad-pair-keyboard.log`.
3. Other cases:
   - Nothing found, or several keyboards nearby: `ssh linux@172.16.42.1 ipad-pair-keyboard scan` lists devices with keyboards marked, then `ipad-pair-keyboard pair XX:XX:XX:XX:XX:XX`.
   - Retry from scratch: `ipad-pair-keyboard forget XX:XX:...`, put the K2 back into pairing mode, pair again.
   - State: `ipad-pair-keyboard status` (controller, paired devices with trusted/connected and their input device).
   - **Manual fallback** if the helper misreads something (it runs this same sequence, so its transcript in `/run/ipad-pair-keyboard.log` still shows where it went wrong):
     ```
     ssh -t linux@172.16.42.1 sudo bluetoothctl --agent KeyboardDisplay
     default-agent
     pairable on
     scan on                      # K2 in pairing mode; wait for "[NEW] Device XX:... Keychron K2"
     scan off
     pair XX:XX:XX:XX:XX:XX       # type the printed "Passkey: NNNNNN" on the K2, then Enter
     trust XX:XX:XX:XX:XX:XX
     connect XX:XX:XX:XX:XX:XX
     quit
     ```
     Then `ssh linux@172.16.42.1 ipad-pair-keyboard keys` to confirm.
   - Debug trace: in a second SSH session `sudo btmon -w /tmp/bt.snoop` while pairing; then `scp linux@172.16.42.1:/tmp/bt.snoop docs/test-logs/` and `sudo journalctl -b -u bluetooth`.
4. Check that keys arrive (before any desktop): `ssh linux@172.16.42.1 ipad-pair-keyboard keys 20`, then type on the K2 for 20 s. It prints each key (`KEY_A down`, ...) from the keyboard's event devices. F1 should print `KEY_F1`, not `KEY_BRIGHTNESSDOWN` (fnmode=2). (The image has no `libinput`/`evtest` CLI; the decoder was **verified** in QEMU on a uinput keyboard.)
5. **Reconnect:** `ssh linux@172.16.42.1 'sudo systemctl restart bluetooth'`, press a key on the K2, then `ipad-pair-keyboard status` → `connected=yes`. After the next reboot the same: the device is bonded and trusted and the controller powers on at boot (`AutoEnable=true`), so the K2 reconnects when a key wakes it. That needs the iPad's Bluetooth address to stay the same across boots: it was `00:00:00:00:00:00` (Apple OUI) in device test 1, so it most likely comes from the device tree (inferred); compare `btmgmt info` after a reboot.

What QEMU could and couldn't show: it has no Bluetooth controller. **Verified** there against the image's bluetoothd 5.87: the helper's `bluetoothctl` session (KeyboardDisplay agent registered, made default, commands and replies parsed), its error paths, and the key decoder. The scan/pair/passkey/trust exchange with a real keyboard is **inferred** until this step runs; the transcript in `/run/ipad-pair-keyboard.log` shows what happened.

## 7. Desktop on the iPad screen (criterion 3)

From round 3 the desktop starts by itself (`graphical.target` is the default). On an older image: `ssh linux@172.16.42.1 'sudo systemctl set-default graphical.target && sudo systemctl start sddm'` (or `scripts/omarchy/hot-apply.sh`). To boot without it: `systemd.unit=multi-user.target` in the bootargs.

SDDM autologins `linux` into Omarchy's session: Hyprland on simpledrm + llvmpipe, scale 2, effects off, **portrait** (`ipad_transform = 0`, upright like the boot console; landscape below). Expect it to be slow (2 A9 cores rendering 1536×2048 in software): perhaps a minute before the bar appears, and a few frames per second (inferred). On the K2 (Cmd = SUPER): `SUPER+RETURN` terminal, `SUPER+SPACE` Omarchy menu, `SUPER+ALT+SPACE` apps, `SUPER+K` keybindings, `SUPER+W` close window.

Capture:
```sh
ssh linux@172.16.42.1 'export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1
  pgrep -a Hyprland; hyprctl -i 0 monitors | head -12; top -bn1 | head -15
  grep -iE "render node|gbm|egl|llvmpipe|dumb|error" $XDG_RUNTIME_DIR/hypr/*/hyprland.log | head -30
  grim /tmp/ipad-desktop.png'
scp linux@172.16.42.1:/tmp/ipad-desktop.png docs/test-logs/2026-10-22-omarchy-desktop.png
```

(`hyprctl -i 0` and `grim` with these two variables were **verified** over SSH in QEMU.) Expected: `1536x2048` with `transform: 0`, which confirms the portrait panel (inferred so far). It's not a pass/fail gate: if it says `2048x1536`, the panel is landscape-native and the landscape advice below swaps (0 becomes the landscape setting). A photo of the screen helps too.

**Landscape** (keyboard in front, home button on the right): `ssh linux@172.16.42.1 "sed -i 's/^local ipad_transform = .*/local ipad_transform = 3/' ~/.config/hypr/ipad.lua && XDG_RUNTIME_DIR=/run/user/1000 hyprctl -i 0 reload"`. Use `1` for the home button on the left, `0` for portrait. **Verified** in QEMU: this edit plus `hyprctl reload` rotates the live desktop, and transform 1 puts the desktop's top on the panel's left edge. That 3 means "home button on the right" on the iPad is **inferred** from the portrait panel being upright with the home button at the bottom.

**Keyboard layout** (default US ANSI). The one place is `XKBLAYOUT` in `/etc/vconsole.conf`, which Omarchy's Hyprland config reads at login (console keymap too). One line changes it, then restart the session (`sudo systemctl restart sddm`):
```sh
ssh linux@172.16.42.1 'sudo localectl set-x11-keymap de pc105+inet "" terminate:ctrl_alt_bksp'   # e.g. German
```
(**verified** in QEMU: it rewrites both `KEYMAP` and `XKBLAYOUT`.) For a new image, `OMARCHY_KEYMAP=de scripts/omarchy/build-rootfs.sh --fresh`.

Stop the desktop: `ssh linux@172.16.42.1 'sudo systemctl stop sddm'`. To boot straight into it once it works: `sudo systemctl set-default graphical.target`.

## 8. Shut down

```sh
ssh linux@172.16.42.1 'sudo systemctl poweroff'
```

Then Ctrl-C `serve-root.sh`. A reboot goes back to iOS (tethered boot). Don't stop the server while the iPad still runs from it.

## Round 3: device commands (live iPad, no reboot)

```sh
scripts/omarchy/hot-apply.sh                    # helpers, BT units+timer, Hyprland layer, graphical.target; prints a check
scripts/omarchy/ssh.sh 'grep -c trigger.toggle.ipad ~/.config/omarchy/extensions/omarchy-menu.jsonc'   # 8; then open SUPER+SPACE once: the menu must still open
```

**Rotation:** `scripts/omarchy/ssh.sh 'ipad-rotate; ipad-pointer; ipad-rotate portrait; sleep 5; ipad-rotate landscape'` (prints `landscape`, `visible`; the screen turns and back). SUPER+ALT+R cycles.

**K2 reconnect** (each step drops nothing unless noted):
```sh
scripts/omarchy/ssh.sh 'sudo ipad-bt-doctor'                  # bdaddr, raw scan_enable, kernel accept list, verdict
scripts/omarchy/ssh.sh 'sudo ipad-bt-doctor --trace-restart'  # DROPS the K2: replays the boot sequence under btmon, workaround held off
```
Read the trace output: K2 on the accept list + `page=off` + no Write Scan Enable after Set Powered → kernel (`hci_powered_update_sync`; look for a failed command in "did not succeed"); K2 absent from the accept list → BlueZ's load path; `page=on` → the keyboard isn't paging: `scripts/omarchy/ssh.sh 'sudo timeout 30 btmon | grep -A3 "Connect Request"'` while pressing a key. With the K2 disconnected, `sudo ipad-bt-doctor --probe` gives a second view. If an SSH drop interrupts `--trace-restart`: `sudo systemctl unmask --runtime linux-a9-bt-pagescan.service && sudo systemctl start linux-a9-bt-pagescan.timer linux-a9-bt-pagescan.service`. Then the real test: power the K2 off and on (or wait for it to sleep), press a key, `ipad-pair-keyboard status` → connected; and after a reboot. `journalctl -b -u linux-a9-bt-pagescan` shows each time connectable had to be turned on and what page scan was then.

**Audio (hardware volume):**
```sh
scripts/omarchy/ssh.sh 'pactl list sinks | grep -E "Name:|Active Port|Volume:"; amixer -c0 sget Speaker | tail -1
  pactl set-sink-volume @DEFAULT_SINK@ 50%; amixer -c0 sget Speaker | tail -1; pactl set-sink-volume @DEFAULT_SINK@ 70%'
```
Pass: Active Port `analog-output-speaker` and the `Speaker` value moves with the sink volume. On the K2 (Mac mode, fnmode=2) the volume keys are **fn+F11/F12** (fn+F10 mute); the OSD should show. If the port is `analog-output` (software volume) instead, a WirePlumber rule is the fix (not written yet).

**Desktop at boot:** after the next reboot, nothing to start: `scripts/omarchy/ssh.sh 'systemctl get-default; systemctl is-active sddm'`.

## Recovery

| Symptom | Do |
|---|---|
| Screen says `RESCUE: no NBD root` (after `omarchy.wait`) | Is `serve-root.sh` running, and did it print "172.16.42.2 is up"? Fix that, then in the rescue shell (`screen /dev/cu.usbmodem* 115200`, or `telnet 172.16.42.1`) run `touch /run/omarchy-continue`. Log: `/HL_init.log`. |
| The Mac never gets 172.16.42.2 | In the rescue shell: `ip addr show usb0`, `pidof unudhcpd`, `ls /sys/class/udc`. If the gadget is up but DHCP fails, set the Mac's interface by hand: `sudo ifconfig enX 172.16.42.2/24`. |
| `NBD server answered but the connect failed` | Another client may still hold the single slot (a stale connection from an earlier boot). Restart `serve-root.sh`, then `touch /run/omarchy-continue`. |
| Manual NBD in the rescue shell | Use **`/sbin/nbd-client`**: plain `nbd-client` is BusyBox's ioctl-only applet, which has no `-l`/`-N`. `/sbin/nbd-client -N omarchy 172.16.42.2 10809 /dev/nbd0 && mount /dev/nbd0 /newroot`. |
| systemd hangs or SSH never answers | USB serial: root autologin console after `switch_root` (`screen /dev/cu.usbmodem* 115200`). Check `systemctl list-jobs`, `journalctl -b -p err`. Without a serial console either, reboot with `omarchy.rescue` and inspect from the initramfs. |
| `dmesg` shows `Possible stuck request` | The Mac stopped answering; I/O waits for it rather than failing. Check `serve-root.sh`. |
| Keyboard pairs but types nothing | `zgrep -e CONFIG_UHID= -e CONFIG_HID_APPLE= /proc/config.gz` must show `=y` for both (the integ kernel needs the fragment), and `ls -l /dev/uhid`. Then `sudo journalctl -b -u bluetooth --grep=hid` and `ipad-pair-keyboard keys`. |
| Keyboard doesn't reconnect after a reboot | `ipad-pair-keyboard status` (trusted? paired?), `btmgmt info` (did the controller's address change?). Re-pair with `forget` + pairing mode. |
| Desktop never appears | `sudo journalctl -b -u sddm`, `tail -50 /run/user/1000/hypr/*/hyprland.log`, `ls -l /dev/dri` (`card0` = simpledrm). |
| Want the known-good shell back | The HoolockRD payload: `TREE=integ scripts/mkpayload.sh` (no argument means HoolockRD). |
| Image in a bad state | `cp -c build/omarchy/rootfs.pristine.img build/omarchy/rootfs.img`, or rebuild with `scripts/omarchy/build-rootfs.sh --fresh`. |

Never run two servers, and never serve while a build container has the image loop-mounted: two writers corrupt the ext4.

## Offline results (QEMU/HVF, before any device boot)

`scripts/omarchy/qemu-smoke.sh` on the Mac: QEMU `virt` + HVF, 2 cores, 2 GB, a 16K-page kernel built from `build/kernel-omarchy/config` (iPad config + fragment) plus `scripts/omarchy/qemu/qemu-virt.fragment`. Reports: `build/omarchy/qemu/report-{disk,nbd}.txt`. The image under test is never written (qcow2 overlay / `qemu-nbd --snapshot`), so each run is a real first boot.

| Run | Result (**verified**) |
|---|---|
| `--nbd`: the real initramfs, `serve-root.sh --bind 127.0.0.1 --snapshot`, NBD over QEMU's NIC (`omarchy.ip=`), `switch_root` | `running`, **no failed units**, multi-user after 7.0 s of userspace (final image; critical chain is sshd ← NetworkManager ← dbus); no `qemu-nbd` left behind afterwards; root `/dev/nbd0 ext4 rw,noatime`; PAGESIZE 16384; `omarchy-hw-platform` = `aarch64`; SSH + `sudo -n` OK; usb0 still administratively UP after `switch_root` with its stable MAC (`NO-CARRIER` only because QEMU has no USB host). |
| `--disk --hyprland`: first boot from a virtio disk, then SDDM | `running`, no failed units. `omarchy-provision-hardware` ran all 53 steps, made the keyring, logged "No mkinitcpio installed, so this machine's initramfs is built elsewhere; nothing to rebuild" and "Deferred hardware setup is complete" (journal), and disarmed itself (`/var/lib/omarchy/image` = `lock`, `target.booted`). Hyprland 0.56.2 + Omarchy's Quickshell bar on bochs-display (KMS only, no render node) through llvmpipe, **no config-error banner** (`screen-disk.png`). Multi-user takes 91 s here only because disk mode has no gadget: `usb0`/`ttyGS0` time out after 90 s. |
| Round-2 items (both runs) | zswap `N`; `hid_apple` fnmode `2` (tmpfiles); charger unit enabled and skipped by `ConditionFirmware=device-tree-compatible(apple,j71t)` (that condition evaluates true for `linux,dummy-virt`, false for `apple,j71t`); bluetooth.service enabled + active, `AutoEnable=true`; `brcm/BCM.apple,j71t.hcd` and `apple/j71t-n1.bin` in `/usr/lib/firmware`, readable; `XKBLAYOUT=us`; `grim`, `jq`, `hyprctl` present. |
| Desktop commands over SSH | `hyprctl -i 0 monitors`, `grim` (with `WAYLAND_DISPLAY=wayland-1`), and editing `ipad_transform` + `hyprctl -i 0 reload` (desktop rotated live, `transform: 1` reported, `screen-nbd-transform1.png`). |
| Pairing helper | Session with bluetoothd 5.87 (agent, default agent, `scan on` → "No default controller available" handled), CLI errors, `keys` decoder on a uinput keyboard (`KEY_F1 down/up`, `KEY_A down/up`, clean exit when the device goes away). |

Bugs that QEMU found in the round-1 boot path, all fixed:
- **The initramfs could never reach the NBD server.** BusyBox's `--install -s` puts its own `nbd-client` applet (ioctl only, no `-l`) in `/usr/sbin`, ahead of the real `/sbin/nbd-client` in `PATH`. The probe failed silently and the device would have dropped to the rescue shell after 600 s. The init now calls `/sbin/nbd-client`, and the initramfs build rejects a bare `nbd-client` call.
- The stable USB MACs were never applied (`Couldn't set dev_addr: Resource busy`): u_ether refuses address writes while the function is linked, and HoolockRD's helper links it first. Now the function is unlinked, the addresses set, then relinked, all before the UDC binds.
- `serve-root.sh` left `qemu-nbd` running when it was stopped (its `serve` function ran in a subshell). On the device path that would have kept the old server holding port 10809 when the 20 s link-loss restart kicks in. `serve` now `exec`s.
- First-boot provisioning failed (`mkinitcpio: command not found`), making every boot `degraded`: patch 0003.

## Open risks

1. **The integ kernel must include the fragment** (step 0.1). Without it the system still boots (inferred), but the K2 types nothing: BlueZ 5.87's classic HID path needs `CONFIG_UHID`.
2. **Pairing on real hardware is untested** (step 6). Known unknowns: whether the K2 asks for passkey entry (handled) or a legacy PIN (handled with a 4-digit PIN), and whether bluetoothd brings the controller up with LE on (the helper turns it on if not).
3. **Reconnect after reboot** needs a stable controller address. Device test 1 showed `00:00:00:00:00:00` (Apple OUI, so most likely from the device tree, inferred); if it changes per boot, the pairing is lost at each boot.
4. **First boot over USB 2.0 NBD is slow** (inferred). In QEMU on the Mac's loopback, userspace took 7.6 s; on the A9 over USB expect minutes for the first boot (keyring generation, 53 deferred steps). The cheap levers, if needed, are masking services the iPad can't use yet (cups, avahi, docker.socket); they are Omarchy defaults and left on.
5. **A dropped NBD connection is fatal.** With no I/O timeout, a stalled Mac only stalls the iPad, but a broken TCP connection (server killed, USB re-enumerated for long enough) fails root I/O and the iPad needs a reboot. There's no automatic reconnect in netlink mode yet.
6. **`--shared=1` and stale connections.** An iPad that resets leaves a dead connection on the Mac. `serve-root.sh` restarts once the link has been down for 20 s, which covers reboots; a shorter USB flap keeps the old session, by design.
7. **Edge channel = moving target.** The image tracks `omarchy-dev` (be54892 in both round-1 and round-2 builds). An `omarchy update` replaces patched files; the pacman hook re-applies the three patches and warns loudly if they no longer apply. There's no internet on the device anyway (no route; Internet Sharing on the Mac would be needed).
8. **Temporary security posture** (orchestrator-approved): no passwords, NOPASSWD sudo for `linux` (`/etc/sudoers.d/90-linux-a9-temp`), root autologin on the USB serial console, and an unauthenticated telnet root shell in the initramfs (HoolockRD's behaviour, gone after `switch_root`). All need physical or USB access. ufw allows only 22/tcp and 67/udp on usb0.
9. **Desktop performance** (inferred): llvmpipe on two cores at 1536×2048, plus Quickshell, will be slow; Chromium/Electron are the worst. 1.95 GB of RAM with zram swap (`zram-size = ram`, zstd; zswap now off) is tight for Chromium.
10. **Orientation.** Portrait is the default because it's the panel's native, known-upright orientation. Which landscape transform puts the home button on the right is inferred (3); a photo settles it.
11. **Charging** relies on the explicit 2.4 A request (initramfs + `linux-a9-charger-limit.service`, j71t only) until the charger driver negotiates by itself; drop both then. The request asserts the owner's Mac port: on a weaker source it would over-ask (the driver's own guards still apply). Whether it is accepted depends on the periph round in the integ kernel (round 4 refused it, round 5 accepts it again per `docs/periph/round5.md`).
