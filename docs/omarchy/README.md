# Omarchy workstream (rounds 1–3)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

## What's built

| Artifact | How | Size |
|---|---|---|
| `build/omarchy/rootfs.img` | `scripts/omarchy/build-rootfs.sh --fresh` | 24 GiB sparse ext4 (label `omarchy-root`), 8.5 GiB on disk, 842 packages, never booted (pristine) |
| `build/omarchy/initramfs-nbd.gz` | `scripts/omarchy/build-initramfs.sh` | 3.1 MB (with this unit's firmware), one gzip stream of one cpio (checked by the build) |
| `build/kernel-omarchy/` | `scripts/omarchy/build-kernel.sh` | HoolockLinux `hoolock` 6831bc70 (7.3.0-rc1), `hoolock-16k.config` + `configs/omarchy.fragment` (80/80 options), `Image.gz` 9.8 MB. The device boots the **integ** kernel instead, with the same fragment merged by `scripts/integrate.sh`. |
| `build/omarchy/initramfs/nbd-client` | `scripts/omarchy/build-nbd-client.sh` | nbd 3.26.1, static aarch64 (musl), netlink |
| `patches/omarchy/0001–0003` | `git format-patch` from `build/omarchy/src/omarchy` | see [`patches/omarchy/README.md`](../../patches/omarchy/README.md) |

Private inputs (the Mac's SSH public key, `build/firmware/lib/firmware`) only go into the gitignored image and initramfs.

## Rootfs: how Omarchy was installed

**Source of truth: Omarchy's own installer.** `omacom/omarchy-iso` owns installation: its orchestrator (`configs/airootfs/usr/share/omarchy-iso/orchestrator/main.py`, `phases_impl.py`) pacstraps a base, installs `omarchy-settings` before `useradd` (so `/etc/skel` seeds the user), installs the runtime plus `omarchy-pkg-defaults <platform>`, then runs `omarchy-apply-system --install-user USER --first-install` as root and `omarchy-provision-user --force --first-install` as the user in the target chroot, with a fixed environment (`OMARCHY_USER_NAME`, `OMARCHY_USER_EMAIL`, `OMARCHY_MIRROR`, ...). `scripts/omarchy/rootfs/in-container.sh` replays that phase list in `arch-chroot` on a loop-mounted image, inside a privileged arm64 container made from the Arch Linux ARM tarball (`scripts/omarchy/fetch-alarm.sh`, md5 and build-key signature checked, **verified**).

**Image build, not a disk install.** Before anything from Omarchy runs, the build writes Omarchy's image manifest `/var/lib/omarchy/image/target` (`format=1`, `platform=aarch64`, root-owned 0644). That is Omarchy's contract for building away from the target machine (`install/helpers/image-target.sh`). While the manifest exists, `omarchy-hw-platform` answers from it, and `omarchy-apply-hardware` queues every hardware step for the machine's first boot instead of running it (`omarchy-provision-hardware.service`).
- **Verified:** inside the build, `omarchy-hw-platform` printed `aarch64` (manifest), and `omarchy-apply-system` reported "Image build for aarch64: deferred 53 hardware steps to first boot".
- **Inferred, from reading each step's gate** (`build/omarchy/reports/deferred-steps.txt`): on the iPad only `network.sh` (retires systemd-networkd), `bluetooth.sh` (enables BlueZ) and `set-wireless-regdom.sh` act. Everything else is gated on DMI, PCI IDs or another platform. `CONFIG_PCI=y` gives an empty `/sys/bus/pci`, so the ungated `lspci` calls see nothing. First boot also makes this machine's pacman keyring (gpg key generation on the A9).

**Channel and runtime: edge, `omarchy-dev`.** **Verified** from Omarchy's own source (`install/helpers/pacman.sh` at v4.5.0-rc1): "Omarchy publishes aarch64 packages on edge alone so far". The stable package (4.0.6) predates aarch64 support entirely: it has no `omarchy-hw-platform`, no image manifest and no aarch64 templates. So the image uses what the nightly edge aarch64 ISO installs: `omarchy-dev`/`omarchy-settings-dev` 4.0.0.r7218.gbe54892 (Omarchy commit be54892, 3 commits behind `quattro` HEAD, with identical detector and image code), `omarchy-nvim`, `omarchy-keyring`, and the edge pacman templates. Omarchy's signing key from `omarchy-keyring` matches `omarchy-iso`'s `builder/omarchy.gpg` (40DF B630 FF42 BCFF B047 046C F013 4EE6 80CA C571, **verified**).

**Package set: Omarchy's, not hand-picked.** The set is `omarchy-pkg-defaults aarch64` (base + aarch64 lists) after the ISO's own aarch64 substitutions (`builder/aarch64-package-filter.sh`: `nvim`→`neovim`, `vi`→`ex-vi-compat`, x86-only packages dropped). 158 packages on top of the runtime, listed in `reports/installed-default-set.txt`. Key versions (**verified**, `reports/packages.txt`): Hyprland 0.56.2, Aquamarine 0.15.1, Mesa 26.2.5, quickshell 0.3.2, systemd 262.1, sddm 0.21.0, uwsm 0.27.1, NetworkManager 1.58.1, Chromium 153, Docker 29.9.

**Deliberate differences from a disk install** (the iPad boots m1n1 → Linux from the Mac: no UEFI, no Limine, no kernel package):
- No kernel or headers package (`linux-omarchy`), no Limine/efibootmgr/snapper stack, no hibernation or factory snapshot, no `linux-firmware` (PC firmware). With no kernel package there's no `mkinitcpio` either, and Omarchy's first boot would try to rebuild the initramfs after the deferred steps change one of its inputs (here `fix-fkeys.sh`'s `/etc/modprobe.d/hid_apple.conf`). That failed in round 1 (`mkinitcpio: command not found`, `degraded` on every boot); patch 0003 makes it skip the rebuild on a machine with no initramfs builder.
- ext4 root, not btrfs, so any workstream's kernel can mount it (BTRFS isn't in `hoolock-16k.config`). Omarchy's `config/snapper.sh` assumes btrfs, which patch 0002 handles.
- Default target `graphical.target` (round 3; rounds 1–2 used `multi-user.target`): SDDM autologins into Hyprland at boot. sshd, the DHCP unit and the serial console are in `multi-user.target` underneath, so `systemd.unit=multi-user.target` on the kernel command line boots without the desktop.

**Platform it believes it's on.** In the image build: `aarch64`, from the manifest (**verified**). On the iPad after first boot, the manifest is retired and the live device tree (`apple,j71t apple,s8003 apple,arm-platform`) decides. Stock Omarchy would say `aarch64-apple` there. With patch 0001 it says `aarch64`. **Verified** with fixtures in the built image, as a normal user: the iPad tree gives `aarch64`, and an M1 (`apple,j274 apple,t8103`) still gives `aarch64-apple`.

> Private graphics-dependent excerpt omitted from this source snapshot.

The image ends with Omarchy's exact edge `pacman.conf`. The build turns pacman's Landlock sandbox off only while it runs, because Docker Desktop's kernel has no Landlock. The build checks that the final file matches Omarchy's template byte for byte.

## Image packaging (reproducible build)

One command builds the whole image from pinned inputs: the Arch Linux ARM tarball (md5 + signature checked), Omarchy's edge repo (`omarchy-dev`; its version lands in `reports/packages.txt`), this repo's `scripts/omarchy/rootfs/`, `patches/omarchy/`, and the private firmware in `build/firmware/lib/firmware` (plus `EXTRA_FW` in `build-rootfs.sh`). Package versions follow the edge and Arch Linux ARM mirrors on the day, so two builds on different days differ in versions but not in steps.

```sh
# The iPad runs from rootfs.img over NBD: build the next image under another name.
OMARCHY_ROOTFS=rootfs-next.img scripts/omarchy/build-rootfs.sh --fresh   # ~4 min warm, log in build/omarchy/logs/
OMARCHY_ROOTFS=rootfs-next.img scripts/omarchy/qemu-smoke.sh --disk --hyprland   # report in build/omarchy/qemu/report-disk.txt
# Swap at the next reboot (the iPad must be off or in m1n1, serve-root.sh stopped):
mv build/omarchy/rootfs.img build/omarchy/rootfs-prev.img && mv build/omarchy/rootfs-next.img build/omarchy/rootfs.img
```

- Always `--fresh`: a resume skips every phase whose marker is in the image, so changed files under `rootfs/` would not land.
- The build refuses an image that `qemu-nbd` serves, and `qemu-smoke.sh` refuses one served read-write; the smoke test boots a throwaway qcow2 overlay and never writes the image.
- A fresh image is a first boot on the iPad again: new SSH host key, Omarchy's deferred hardware steps, and **no Bluetooth pairing or Wi-Fi password** (they live in `/var/lib/bluetooth` and `/etc/NetworkManager/system-connections` of the old image). Save them from the running iPad before the swap, `scripts/omarchy/ssh.sh 'sudo tar -C / -czf - var/lib/bluetooth etc/NetworkManager/system-connections' >build/omarchy/private-state.tgz`, and restore them on the new image's first boot: `scripts/omarchy/ssh.sh 'sudo tar -C / -xzf - && sudo systemctl restart bluetooth NetworkManager' <build/omarchy/private-state.tgz`. Or re-pair.
- To change a running iPad without a rebuild or a reboot: `scripts/omarchy/hot-apply.sh` (the linux-a9 helpers, units and Hyprland layer, through the iPad itself; `--check` only reports).
- Disk: each image is 24 GiB sparse, ~8.5 GiB used. Keep at most the live image, the next one and one fallback.

## Boot path

```
Mac                                   iPad (m1n1 → Linux 7.3, initramfs-nbd.gz)
                                      /init: USB gadget NCM+ACM (one bind, stable MACs)
                                             usb0 172.16.42.1/24, unudhcpd → Mac gets .2
serve-root.sh waits for 172.16.42.2   ACM getty + telnetd (rescue shells, whole initramfs)
qemu-nbd 172.16.42.2:10809 'omarchy'  nbd-client -l (probe), then netlink connect, -b 4096
                     ◄──── NBD ────   mount /dev/nbd0 (ext4, rw) at /newroot
                                      stop shells + DHCP; move /proc /sys /dev /run /config
                                      switch_root → systemd → multi-user, sshd,
                                      linux-a9-usb0-dhcp (dnsmasq), serial-getty@ttyGS0
```

- **Initramfs** (`scripts/omarchy/initramfs/init`): HoolockRD's `init_functions.sh` (GPLv3) is sourced unchanged for the configfs gadget, logging and getty. BusyBox 1.38.0, busybox-extras and unudhcpd come from Alpine edge, as HoolockRD's own build takes them. **Unlike HoolockRD**, ACM is added before the first UDC bind (one enumeration, serial console from the start), and the NCM MACs are derived from the device serial number so the Mac sees the same interface every boot. The function is unlinked from its config while the MACs are written, because u_ether refuses them (EBUSY) while linked (**verified** in QEMU: round 1's writes failed, round 2's MAC is applied). On j71t it also requests 2.4 A from the USB host for the charger. Any failure goes to a rescue loop that keeps the shells up. From a shell, `touch /run/omarchy-continue` retries the boot and `touch /run/omarchy-hold` stops before `switch_root`. Kernel arguments: `omarchy.nbd=HOST:PORT/EXPORT`, `omarchy.wait=SECONDS` (600), `omarchy.rescue`, `omarchy.init=PATH`, `omarchy.ip=IF:ADDR/LEN:GW` (QEMU tests; a second telnetd listens on ADDR).
- **The real nbd-client is always called as `/sbin/nbd-client`.** BusyBox's `--install -s` adds its own ioctl-only `nbd-client` applet in `/usr/sbin`, ahead of `/sbin` in `PATH`. In round 1 that applet answered the `-l` probe, which failed silently, so the device would have waited 600 s and dropped to rescue (**verified** in QEMU). `build-initramfs.sh` now rejects any bare `nbd-client` call in the init.
- **nbd-client**: upstream 3.26.1, static. **Verified** from its source: in netlink mode it hands the socket to the kernel (`NBD_CMD_CONNECT`) and returns, so no process is left for systemd's shutdown to kill. `-L -p -m` (ioctl, reconnecting, systemd-marked) is the fallback. No I/O timeout is set: **verified** in `drivers/block/nbd.c` (`nbd_xmit_timeout`), a request without a timeout is logged as "Possible stuck request" and kept, never failed. A slow Mac stalls the iPad but doesn't corrupt it. HoolockRD's BusyBox `nbd-client` applet can do neither.
- **Server** (`scripts/omarchy/serve-root.sh`): native `qemu-nbd` 11.1.2 (Homebrew), `--cache=writeback --aio=threads --discard=unmap --detect-zeroes=unmap --shared=1 --persistent`, bound only to 172.16.42.2 (or 127.0.0.1 for tests, and never 0.0.0.0, because the export is an unauthenticated read-write root). A writeback cache means the Mac's page cache absorbs writes, and the iPad's flushes (ext4 commits) become `fsync`s of the image. An iPad reset sends no FIN, so its dead connection would hold the single client slot. The script therefore restarts the server whenever 172.16.42.2 has been gone for 20 s, which every reboot causes. `serve` `exec`s `qemu-nbd`, so killing the job stops the server itself: in round 1 it killed a wrapper subshell and left `qemu-nbd` running (**verified**: TERM now stops both). It refuses to start while a build container could have the image mounted. The macOS Application Firewall is off on this Mac (**verified**, `socketfilterfw --getglobalstate`), so no prompt appears.

## Kernel fragment

`configs/omarchy.fragment`: 80 options, tree-agnostic. `scripts/omarchy/build-kernel.sh [TREE]` merges it into any tree's existing `.config` with `merge_config.sh -m` + `olddefconfig`, and `scripts/omarchy/check-fragment.sh [CONFIG]` lists any option that didn't survive. **Verified:** 80 checked, 0 mismatched in `build/kernel-omarchy/config`.

> Private graphics-dependent excerpt omitted from this source snapshot.

16K pages stay. **Verified** audit of the built image (`scripts/omarchy/rootfs/elf-page-audit.py`, `reports/elf-16k-audit.txt`):
- 0 of 6112 aarch64 ELF files have a PT_LOAD alignment below 16K.
- jemalloc users: quickshell (Omarchy's shell) and fd link the system libjemalloc, and Arch Linux ARM's jemalloc 5.4.0 reports `arenas.page=65536` (LG_PAGE 16), so it works on 4K, 16K and 64K kernels. js140/libmozjs and mariadb embed their own page-size-agnostic allocators.

There are no 16K blockers in the package set as installed. Third-party `-bin` software installed later (AUR, Omarchy's optional apps) can still bring 4K-aligned binaries; rerun the audit on the image.

## Open risks

See the end of [`test-plan.md`](test-plan.md).
