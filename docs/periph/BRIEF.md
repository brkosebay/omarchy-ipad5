# Peripherals workstream brief (BT, battery, ALS, touch recon, firmware)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Device: iPad 5 j71t (iPad6,11, S8003 A9, iPadOS 16.7.10 20H350). HoolockLinux
7.3.0-rc1 boots from RAM (`PLAN.md` P1). Template: Pauli1Go/HoolockLinux
(A10 iPad 7 j172 + iPhone 7 Plus d111). It has the same Bluetooth chip, gauge
and ALS parts as our iPad (BCM4355 / `bluetooth,n88` on uart3, bq27540 over
HDQ on uart5, charger `k48`, CT819 ALS on i2c2). Its README:
`gh api repos/Pauli1Go/HoolockLinux/contents/README.md?ref=hoolock`.
Touch bring-up notes are at
`https://github.com/Pauli1Go/HoolockLinux-bringup-docs` (docs/touch.md).

## Relevant template commits (Pauli1Go/HoolockLinux)

- Bluetooth: `3444812cd9` hci_bcm Apple J172 BCM4355C1, `460a07c07b` t8010
  UART3, `3a7932fdec` j172 Bluetooth DT (and the d111 equivalents
  `363034f234`, `dae61536e1`, `ebbd5359a0`, `dea7a97c9a`).
- Battery: `17375a93ad` w1-uart muxed HDQ, `f81edfc437` binding,
  `089328e971` bq27xxx cached HDQ, plus the j172 battery/charging/power-status
  commits. Find them on branches `hoolock`/`dev`.
- ALS: `c241a321fe`, `7182cde4fa`, `520dcd8b7d`, `56433d234a` (tsl2583 / Apple
  CT819), `ae101dd0de` (ipad7 DT).
- Touch (A10, a different protocol generation from ours): branch `touch`, with
  `3cf006a4c6` SIO core, `720a41eb5a` SIO DMA, `4058c482b4` T8010 SPI,
  `89856a57fd` touch clock, `da36623d44` apple_z2 J172, `a2681b4366` DT.

## ADT facts (`docs/adt/j71t.txt` vs `j172.txt`)

- Touch: j71t `spi3/multi-touch` is `multi-touch,j72` (irq 65,
  `multi-touch-calibration = syscfg/MtCl`). j172 is `hid-transport,spi`. So
  the protocol generation differs and touch needs reverse engineering. Both
  have `sio` + `sio-dma`.
- Audio is out of scope here (CS42L81 on spi1 + mca0).

## Round 1 tasks

1. **Bluetooth.** Port to s800-0-3 / j71t: the uart3 DT node, bluetooth node
   (GPIOs and baud rates from the ADT), and the hci_bcm match. Work out the
   exact BCM4355 revision and firmware (`.hcd`) name for this board.
2. **Battery.** bq27540 gauge over HDQ (w1-uart) plus the charger and power
   status. Whatever j172 needed, adapted to the j71t ADT.
3. **ALS.** CT819 on i2c2 (port; check the calibration source).
4. **Firmware inventory.** List every firmware/calibration file Linux will
   need: BT `.hcd`, Wi-Fi bin/txt/clm/txcap for BCM4355 on j71t, multitouch,
   anything else. Note where each lives on iOS 16.7.10 (rootfs paths) or in
   the kernelcache (`build/kc/...`). Write `scripts/periph/pull-firmware.sh`
   to copy them from the jailbroken iPad over SSH into
   `build/firmware/lib/firmware/` in the Linux layout. The orchestrator runs
   it.
5. **Touch recon (docs only unless it's quick).** Identify the iOS
   multitouch kext(s) for j71t in the kernelcache. Work out the transport/
   protocol (Z2 generation, SIO DMA usage) and how it differs from what
   `apple_z2` and the j172 patches handle. Write `docs/periph/touch-recon.md`
   with a concrete implementation plan.
6. Build with `scripts/build-kernel.sh --tree periph`, export patches to
   `patches/periph/` (git format-patch), and write
   `docs/periph/test-plan.md` with exact on-device commands and expected
   output.

## Rules

> Private graphics-dependent excerpt omitted from this source snapshot.

Final report: what was ported, build status, patches, firmware inventory,
touch findings and plan, and the device test plan summary.

## Round 2 (2026-10-22): stage firmware for the first device test

Round 1 results: `docs/periph/README.md` (kernel head `744636325f11` on
`a9/periph`, 36 patches). The orchestrator has since pulled data from the
live iPad over SSH (`scripts/ipad-ssh.sh`):

- `build/firmware/sources/...`: the Wi-Fi dir
  `System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware/C-4355__s-C0/`
  (feara .trx/.clmb/.txcb plus several `P-feara_M-*_V-*__m-*.txt` NVRAM
  variants), `usr/sbin/BlueTool`, multitouch `J71t.mtprops` etc., and
  `build/firmware/bluetooth-candidates.json`.
- `build/firmware/private/IODeviceTree.txt`: the live ADT as **text**
  (`ioreg -l -p IODeviceTree -w0`, Latin-1, properties as `"key" = <hex>`).
  `ioreg -a` (XML) is broken on iOS 16, so `--read-ioreg` can't work. It
  carries the real resolved calibration (`multi-touch-calibration`,
  `wifi-calibration-msf`, MACs, LSCI if present). Also `private/nvram.txt`.

Tasks:
1. Teach `pull_firmware.py` (or a sibling) to parse the text ioreg dump.
   Resolve which BT patchram candidate and which Wi-Fi NVRAM variant match
   **this unit**, using evidence from the live ADT (module/vendor/revision
   properties on the bluetooth and wlan nodes). Stage
   `build/firmware/lib/firmware/` fully: `brcm/BCM.apple,j71t.hcd`,
   `apple/j71t-LSCI.bin`, and brcmfmac names for BCM4355 C0 PCIe on j71t
   (bin/txt/clm_blob/txcap_blob as brcmfmac expects them), even though Wi-Fi
   needs the PCIe stream.
2. `scripts/periph/mkinitramfs.sh`: produce `build/initramfs-fw.gz` =
   HoolockRD `build/prebuilt/initramfs/initramfs.gz` + a gzip'd newc cpio of
   `build/firmware/lib/firmware` (concatenated archives, which the kernel
   accepts). Keep private calibration out of anything committed.
3. Re-check `docs/periph/test-plan.md` against what's actually staged, so the
   orchestrator can run it verbatim with
   `TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz`.
4. Touch (if time remains): continue `touch-recon.md` using the shared
   Ghidra server through `scripts/ghidra.py` (AppleMultitouchSPI/N1 path in
   the kernelcache). It may still be finishing its first analysis, in which
   case calls block. Don't let that hold up tasks 1–3.

Same rules as round 1.
