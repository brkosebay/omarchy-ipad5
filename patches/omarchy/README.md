# Omarchy patches (linux-a9)

> Historical design/test notes. Current support: [status](../../docs/release/STATUS.md); examples may need local firmware/captures.


Four upstream-format patches against `basecamp/omarchy` (default branch `quattro`, written on 46ef11455, branch `linux-a9/idevice-platform` in `build/omarchy/src/omarchy`; the files they touch are identical in be54892, the `omarchy-dev` snapshot the image installs). Not opened as PRs; the text below is ready to paste. All are carried in the image by `/usr/local/sbin/linux-a9-omarchy-patches` and re-applied after Omarchy updates by a pacman hook (`scripts/omarchy/rootfs/files/`). The image build stops if one no longer applies.

| Patch | Files | Test |
|---|---|---|
| `0001-omarchy-hw-platform-a-series-is-not-apple-silicon.patch` | `bin/omarchy-hw-platform`, `test/shell.d/hw-platform-test.sh` | **Verified:** `hw-platform-test.sh` passes with the patch and fails without it (`not ok - the iphone-5s device tree is plain aarch64, never Apple Silicon`), run as a non-root user in an Arch Linux ARM container. |
| `0002-snapper-skip-when-root-is-not-btrfs.patch` | `install/config/snapper.sh`, `test/shell.d/snapper-test.sh` | **Verified:** `snapper-test.sh` passes with the patch and fails without it (`not ok - snapshot configure leaves Snapper alone on a root that isn't btrfs`). |
| `0003-provision-hardware-no-rebuild-without-mkinitcpio.patch` | `bin/omarchy-provision-hardware`, `test/shell.d/image-deferred-hardware-test.sh` | **Verified:** `image-deferred-hardware-test.sh` passes with the patch (36 ok) and fails without it (`not ok - a machine without mkinitcpio finishes the deferred hardware setup`), run as a non-root user in an Arch Linux ARM container with mkinitcpio removed, as in the image. In QEMU, the unpatched image's first boot failed with `omarchy-provision-hardware: line 147: mkinitcpio: command not found`. |
| `0004-shell-remap-panels-only-when-the-screen-origin-moves.patch` | `shell/Ui/ScreenMoveRemap.qml` | **Verified on the iPad 5** (2026-10-27, Hyprland 0.56.2, Quickshell 0.3.2, llvmpipe): before, every 90° rotation blanked the whole screen for 1–2 s (180° turns were fine); with the patch the screen no longer blanks and rotation is quicker (owner A/B, four rotations each). A brief wallpaper rescale glitch remains. Root cause and source references: `docs/omarchy/rotation-blank.md`. Applier extended to `shell/`. |

---

## PR 1: Tell Apple's A-series iPhones and iPads apart from Apple Silicon Macs

HoolockLinux boots mainline-based Linux on Apple's A7–A11 SoCs (iPhones, iPads, iPod touches, Apple TVs, and the T2) through checkm8, pongoOS and m1n1. Their device trees name `apple,<model>` the way an Asahi Mac's does. An iPad 5's root node, for example, is `apple,j71t apple,s8003 apple,arm-platform`.

> Private graphics-dependent excerpt omitted from this source snapshot.

Every such tree names its SoC, and the A-series SoCs (`s5l8960x`, `t7000`, `t7001`, `s8000`, `s8001`, `s8003`, `t8010`, `t8011`, `t8012`, `t8015`, which is the full set in the kernel's `arch/arm64/boot/dts/apple/`) never overlap with a Mac's (`t8103`, `t8112`, `t8122`, `t60xx`). With this patch, an `apple,` tree whose SoC is an A-series one is plain `aarch64`: an ARM machine without a family of its own, per AGENTS.md's platform rules. It isn't a new family, because nothing needs code of its own yet.

- `bin/omarchy-hw-platform`: an `apple,(s5l89xxx|t70xx|s800x|t801x)` token makes the vendor "none". The Apple-and-Qualcomm contradiction check is unchanged.
- `test/shell.d/hw-platform-test.sh`: an iPhone 5s (A7), an iPad 5 (A9) and an iPhone X (A11) are plain aarch64. The existing M1/M2 cases keep asserting `aarch64-apple`.

Tested: `bash test/shell.d/hw-platform-test.sh` passes, and fails on `quattro` without the change.

## PR 2: Skip Snapper setup when / is not btrfs

`install/config/snapper.sh` runs `snapper create-config /` unconditionally, because the ISO always installs onto btrfs. An image built onto another filesystem has nothing for Snapper to snapshot. Our case is an ext4 root that an iPad mounts over NBD from a Mac, built through the image manifest path (`/var/lib/omarchy/image/target`, `omarchy-apply-system`). There, `create-config` fails and takes `omarchy-apply-system` down with it.

- `install/config/snapper.sh`: when `findmnt -no FSTYPE /` isn't `btrfs`, print why and skip the step. It doesn't `exit`, because the file is sourced.
- `test/shell.d/snapper-test.sh`: the fake-bin `PATH` gets a `findmnt` (btrfs unless the test asks otherwise), and a new case checks that an ext4 root gets no Snapper config and no snapper or systemctl calls.

Tested: `bash test/shell.d/snapper-test.sh` passes, and fails on `quattro` without the change.

## PR 3: Skip the first-boot initramfs rebuild when no mkinitcpio is installed

An image's first boot (`omarchy-provision-hardware`) rebuilds the initramfs after the deferred hardware steps whenever one of them changed its inputs, through `limine-mkinitcpio` or else `mkinitcpio -P`. Some machines don't build their initramfs at all: the kernel and initramfs they boot come from outside the root filesystem (a board's own loader, or a payload chainloaded from another machine). Ours is an iPad 5 that boots m1n1 → Linux from a Mac, with an image built through the manifest path and no kernel package, so no `mkinitcpio`.

There, a step that only touches an input (here `fix-fkeys.sh`, which writes `/etc/modprobe.d/hid_apple.conf`) makes the rebuild call `mkinitcpio`, which isn't installed: "mkinitcpio: command not found", the service exits 1, stays armed, and fails again on every boot, so the system is always `degraded`.

- `bin/omarchy-provision-hardware`: when neither `limine-mkinitcpio` nor `mkinitcpio` is present, say that this machine's initramfs is built elsewhere and finish (clear the owed rebuild, disarm the service) instead of failing. Machines with either tool are unchanged.
- `test/shell.d/image-deferred-hardware-test.sh`: a fixture image whose leaves change the initramfs inputs, booted with no `mkinitcpio`, finishes, owes no rebuild and disarms the service.

Tested: `bash test/shell.d/image-deferred-hardware-test.sh` passes, and fails on `quattro` without the change.

## PR 4: Remap panels only when the screen origin moves

`shell/Ui/ScreenMoveRemap.qml` hides a panel, deletes its native window and recreates it 50 ms later whenever the screen moves, so the layer surface lands at the new position. Quickshell's `ShellScreen` emits a single `geometryChanged` for x, y, width and height, so `onXChanged`/`onYChanged` also fire on a size-only change. Rotating a tablet by 90° swaps the output's logical width and height without moving it, and every panel guarded by `ScreenMoveRemap` (bar, background, dock) is torn down and rebuilt. On a software-rendered desktop that is one to two seconds of black after each quarter turn; a 180° turn keeps the size and never blanks.

The patch remembers the screen's origin and restarts the settle timer only when x or y actually changes. A new screen object records its origin without remapping, as before; real moves still remap.

Tested on an iPad 5 (A9, Hyprland 0.56.2, Quickshell 0.3.2, llvmpipe): quarter turns no longer blank the screen and rotation is faster; moving between outputs is unaffected.
