# Touchscreen workstream brief (iPad 5 j71t, `multi-touch,j72` / N1)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Goal: multi-touch input on the iPad 5 under Linux, as a standard evdev/
libinput touchscreen. Hyprland/Omarchy on the device needs it. Kernel side
first. Firmware and calibration come from this unit's own iOS data.

## What's known (2026-10-22)

- Device: j71t (iPad6,11, S8003 A9), iPadOS 16.7.10 20H350. HoolockLinux
  7.3.0-rc1 boots (see `PLAN.md`, `docs/STATUS.md`).
- ADT (`docs/adt/j71t.txt`, live values in
  `build/firmware/private/IODeviceTree.txt`): `/arm-io/spi3/multi-touch`,
  compatible `multi-touch,j72`, interrupts `[65, 0]`,
  `multi-touch-calibration` (real bytes in the live dump; also
  `build/firmware/private/MtCl.bin`). `sio` + `sio-dma` exist. The A10
  iPad 7 uses `hid-transport,spi` instead, a different generation.
- **Prior reverse engineering, read first:** `docs/periph/touch-recon.md`
  (Rounds 1–3) by the peripherals agent. It covers the N1/C1F15,1 path,
  `MTSPIBootloader_N1::performCalibSeq()` at `fffffff005ea8830`, the
  controller-register calibration sequence, the N1 SPI vtable
  (`fffffff006e12fe8`, register read/write slots), the shared HBPP/Z2 packet
  format and ATN_ACK. Raw Ghidra outputs are in `build/periph/ghidra-round3/`.
  The touch firmware asset (57,016 bytes, C1F15,1, version `0x0210.bin`)
  and staging details are in `docs/periph/round2.md` /
  `firmware-inventory.md`. iOS files: `build/firmware/sources/usr/share/
  firmware/multitouch/J71t.mtprops`; the iOS registry dump
  `build/firmware/private/IOService.txt` has the live multitouch driver
  properties (firmware version, etc.).
- Linux starting points: mainline `drivers/input/touchscreen/apple_z2.c`
  (Apple Z2 touch over SPI) and Pauli1Go/HoolockLinux branches `touch` and
  `dt11`. The A10 J172/D11 work covers SIO core + SIO DMA, T8010 SPI, touch
  clock and apple_z2 extensions, but not the N1 generation we have. Its
  bring-up notes are at https://github.com/Pauli1Go/HoolockLinux-bringup-docs
  (`docs/touch.md`). Check what the HoolockLinux tree already has for A7–A11
  SPI (`spi-1,samsung` style controllers) and SIO on s8000/s8003.
- **Ghidra:** the shared server holds the whole analyzed iOS kernelcache,
  saved and persistent. Use the `ghidra` MCP tools or `scripts/ghidra.py`.
  Kexts: `AppleMultitouchSPI` (+ `MultitouchHID` plugin), the
  `MTSPIBootloader_N1` classes, SIO/SPI drivers. **Rename what you
  understand**; the analysis is shared and autosaved every 30 min.

## Round 1 tasks

1. Work out the complete iOS bring-up of this touch controller: power/reset
   GPIOs and regulators, SPI3 controller config (mode, speed, CS), whether
   SIO DMA is required or PIO is enough, firmware download protocol (N1
   bootloader), the calibration sequence (already partly decoded), then the
   runtime report format (contacts → x/y/pressure/IDs) and the interrupt
   (ATN) handshake. Document it in `docs/touch/n1-protocol.md` with Ghidra
   addresses (verified/inferred).
2. Implement it in a kernel tree `touch` (`scripts/build-kernel.sh --tree
   touch`, `--run touch '<cmd>'`). Reuse or extend apple_z2 if the protocol
   fits, otherwise write a clean N1 driver. Add the SPI controller/SIO bits
   the A9 needs, the DT nodes for j71t, firmware loading
   (`apple/<name>.bin` from the staged asset), and calibration from DT/
   firmware (m1n1 doesn't pass `multi-touch-calibration` today; propose
   how). Make every hardware wait bounded, so a failure must never hang boot
   or the shell.
3. Stage the firmware (`build/firmware/lib/firmware/...`, extend the
   existing `scripts/periph/mkinitramfs.sh` approach in your own
   `scripts/touch/`), export patches to `patches/touch/`, and write
   `docs/touch/test-plan.md` (exact on-device commands run by the
   orchestrator via `scripts/linux-run.sh`: dmesg, `/proc/bus/input/
   devices`, `evtest`-style raw event check with BusyBox tools, expected
   output).

## Rules

> Private graphics-dependent excerpt omitted from this source snapshot.

Final report: protocol summary, driver status (builds? what's implemented),
firmware/calibration handling, test plan, and risks.
