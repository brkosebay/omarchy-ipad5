# J71t peripherals

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**Round 6 is ready:** [round6.md](round6.md) fixes the startup fault latch
that prevented automatic fallback after an initial gauge failure. The gauge
retries its first sample every second, reports ENODATA until ready, and
thermal retries quietly. The guarded charger reevaluates every second and
logs deferred decisions. Kernel head `64221b189e`; 55 patches.
**Verified on integ10:** owner 2400-mA input gives +1.16 A charging and valid
charge-now. Automatic 475000 / 400000 limits and startup silence await the
next device test. Current commands/hashes: [test-plan.md](test-plan.md).
Touch belongs to its separate workstream.

USB charging while the iPad stays on the Mac is analysed in
[ds-charging-policy.md](ds-charging-policy.md): how iOS reaches its 2.4 A
(the ADT's `usb-input-limit-max`, not a negotiation), why our `-34` is
self-inflicted policy churn driven by the USB link state and our `-19` is a
gauge error latched as a missing pack, and the driver changes that keep the
budget steady through bus suspend.

The other direction — the Lightning port as a USB **host** — is
[ds-usb-host.md](ds-usb-host.md): host is EHCI/OHCI (the DWC2 here is
device-only), the camera adapter asks for the swap with vendor request 0x51, the
switch is `USB_CTL` + the PHY host trim words, accessory VBUS is the D2365 rail
plus Tristar register 0x05, and the Linux plan starts with one `GHWCFG2` read.

Ambient light and Smart Cover: [ds-als-hall.md](ds-als-hall.md) is the study; the implementation (kernel branch `a9/sensors`,
auto-brightness and cover daemons) and its device test plan are [sensors-test-plan.md](sensors-test-plan.md).

Motion/auto-rotation research: [AOP sensors, round 1](aop-sensors.md) traces
the A9 RTKit/SPU transport, firmware and accel reports, with an orchestrator
probe plan. It also distinguishes the AP-visible CT819 and PMU hall paths.

> Private graphics-dependent excerpt omitted from this source snapshot.

History: [Round 5](round5.md), [Round 4](round4.md), [Round 3](round3.md).
The following Round 1/2 reports are historical.

**Round 2 is ready for the next boot:** see [round2.md](round2.md),
[firmware-inventory.md](firmware-inventory.md) and [test-plan.md](test-plan.md).
The exact YSBC/u/4.1 Wi-Fi set, inferred FearA USI Bluetooth image and this
unit's validated LSCI are staged in `build/initramfs-fw.gz`. The original
Round 1 report below is historical; its then-unresolved firmware/pull
statements are superseded by the Round 2 evidence.

2026-10-22. Device: iPad6,11 / S8003 / J71t, iPadOS 16.7.10 20H350.
This is an **experimental implementation with offline verification**, not
a claim of tested peripheral hardware. The implementation agent did not
contact or boot the iPad, use SSH/telnet/serial, or access its storage.

## What was ported

**Verified — implemented, built and present in the DTB/kernel:**

- Bluetooth: Samsung UART3 plus the Pauli1Go Broadcom power/baud lifecycle,
  `hci_bcm` board match, ROM-default address handling for C0 and C1, and
  the template's HCI advertising/TX-power quirks. J71t uses PMIC GPIO13 for
  enable, AP GPIO142 for wake, AP pins88–91 for UART, IRQ195 and 3-Mbit
  operational rate. `bluetooth0` is published for loader identity fixup.
- Battery: UART5/IRQ197, AP GPIO2 single-wire HDQ, serdev two-stop-bit
  support, UART-backed HDQ (including optional mux support, not attached on
  J71t), and cached bq27xxx telemetry. The experimental
  `apple,j71t-bq27540` match uses the A10 template's BQ27545 register map.
  Gauge control/data-flash writes remain disabled.
- Charger/status: D2365 status and input-limit reads, periodic refresh and
  gauge corroboration. The J71t DT **never selects the J172 input-current
  policy**. There is no invented USB-online supply; Tristar remains an open
  hardware investigation.
- ALS: CT819 at I²C2 address0x39, AP GPIO71 IRQ, the template's calibrated
  TSL2583/CT819 support and arithmetic tests, plus a private raw LSCI firmware
  input (`apple/j71t-LSCI.bin`) for the existing loader. NVMEM is preferred
  when supplied. Invalid/missing calibration fails explicitly.

**Inferred — still requires device evidence:** the UART oversampling layout
on A9 and 115200 ROM rate, adjacent UART RX/CTS pinmux choices, the inherited
gauge and PMIC register semantics, CT819 LSCI v2 format and actual sensor
behavior. DT MMIO/IRQs and named GPIOs are verified from this unit's ADT;
the driver behavior is not verified by that ADT alone.

## A9 differences that matter

**Verified from the two ADTs:** BCM4355/n88, bq27540/HDQ, D2365 and CT819
are shared part identities; the IRQs and AP GPIOs differ. J71t's module is
`feara`, not J172's `rudderb`. `n88` is insufficient to decide C0/C1 or the
module manufacturer. Linux can request `brcm/BCM.apple,j71t.hcd` without
hard-coding that unanswered revision.

J71t has **Tristar CBTL1610 at I²C0/0x1a** and one dock. J172 has Bellatrix
and another dock/control path. Copying the A10 charger policy or Bellatrix
node would be an unsupported hardware assumption. Only read-only D2365
telemetry is opted in here; current/voltage policy is left to existing PMIC
state, so charging performance is not solved.

Touch differs more substantially: J71t's `multi-touch,j72` personality
selects **AppleMultitouchN1SPI, C1F15,1** in the actual 20H350 kernelcache.
J172's HID/HBPP14 setup is not a drop-in implementation. A9 analog touch
power comes from the PMIC and its touch-clock function differs as well.
See [touch-recon.md](touch-recon.md) for evidence and the concrete N1 plan.

## Build and patch delivery

**Verified:** `/src/wt/periph`, branch `a9/periph`, based on Hoolock
`6831bc701` (7.3.0-rc1), head:

```text
744636325f11c5d3aa7e74bd363b49d15ec94d07
```

Kernel `Image.gz` and **104 Apple DTBs** built successfully in Docker,
debug info disabled, clean Git working tree. Outputs:

```text
build/kernel-periph/Image.gz
SHA256 65a328a5629bdecc18d30fbd13d37c91c9966498b6234064ced62959b2465b2f
build/kernel-periph/dtbs/s8003-j71t.dtb
SHA256 ef8a4f4626ebc5f134ca1d6fce6e70685d419d9631ee87d1b7141942081b36e0
build/kernel-periph/config
build/kernel-periph/COMMIT
build/kernel-periph/DIRTY  (empty)
```

The [36-patch series](../../patches/periph/README.md) applies on top of that
baseline. All 36 mail patches were replayed using a separate Git index,
without checking out or changing any other worktree; the result exactly
matches the committed kernel tree (`75a1913b03a721c581d3c68c2acc88d39706dde5`).
No commits were made in the linux-a9 host repository.

DT merge surface: a single include added to `s8003-j71t.dts`, plus new
`s8003-j71t-periph.dtsi`. The new include holds all UART, PMIC child,
pinctrl and I²C2 additions, using a path reference to the existing unlabeled
PMIC. There are no edits to `s800-0-3.dtsi`, shared PMGR definitions, or
the other three iPad5 variants. This is intended to merge cleanly with
PCIe/NVMe work; reconcile duplicate labels only if another series defines
the same UART resources.

Reproduce locally:

```sh
scripts/periph/configure-kernel.sh
scripts/build-kernel.sh --tree periph
scripts/build-kernel.sh --run periph 'python3 /work/scripts/periph/verify-kernel.py'
python3 -B scripts/periph/test-firmware.py
scripts/periph/export-patches.sh
```

**Verified checks:**

- Clean full kernel build and all Apple DTBs (`build/periph/build-final.log`).
- Required drivers actually linked, exact J71t MMIO/IRQ/GPIO/firmware
  properties, no Bellatrix/flash-update opt-in, and no new wiring on the
  other variants (`build/periph/verify-kernel.log`). On ARM64 the Samsung
  driver supports 18 UART slots regardless of hidden `UARTS=4` config.
- Targeted `dt_binding_check` and all-Apple `dtbs_check`, using dt-schema
  2026.9, with no schema/DTB diagnostics after correcting the imported
  D2365 schema. Optional `yamllint` was not installed; this is not a claim
  of having run that linter. Logs: `bindings.log`, `dtbs-check.log` under
  `build/periph/`. Re-run via `scripts/periph/check-bindings.sh`; first-time
  tool setup is `scripts/periph/check-bindings.sh --install`.
- Eight offline firmware tests: inventory versus explicit staging,
  correctly named OTP aliases, truncated HCD rejection, ambiguous duplicate
  rejection, manufacturer mismatch, LSCI staging/checksum failures,
  placeholder rejection and no-connection dry-run (`test-firmware.log`).
- Exported series exactly reproduces the committed tree (`export.log`).

The imported KUnit CT819/CT821 test source is included but was **not run in
a Linux test kernel**. Hardware, suspend/resume and the real iOS pull remain
unverified.

## Firmware and next device pass

[firmware-inventory.md](firmware-inventory.md) lists exact Linux destination
names, candidate iOS source roots, all ADT calibration inputs and the remaining
unknown filenames/revision. `scripts/periph/pull-firmware.sh` performs only
remote reads when the orchestrator runs it, preserves sources, requires an
explicit OTP/module match for staging, and stores manifests/private data
under `build/firmware/`.

**Verified — already extracted offline:** A9 SmartIO `__firmware`, 487488
bytes, SHA256 `da53095ed3364689ffd56f6576f036e4741a98836147a181c335626389ef5a36`,
staged at `build/firmware/lib/firmware/apple/s8000-smartio.bin`. This does
not enable touch. No real HCD, Wi-Fi files or private LSCI/MtCl/PxCl were
obtained by this workstream.

[test-plan.md](test-plan.md) gives the exact build/probe/sysfs/BlueZ/IIO
commands and expected behavior. The first device pass should establish
UART/firmware/identity correctness, battery plausibility against iOS,
several cache refreshes and cable transitions, and calibrated dark/light
response. Touch has its own later sequence: SIO handshake, SPI/clock/N1 ID,
bounded RAM upload, calibration, report discovery, then contact parsing.

## Open risks and integration gates

- **Unresolved:** exact BCM4355 silicon revision, Feara manufacturer/profile,
  actual iOS 16 Wi-Fi/HCD filenames, and the N1 touch firmware asset. These
  cannot be established from `n88` or a list of shipped firmware variants.
- **Inferred:** early UART and GPIO/power sequencing, gauge map and D2365
  status/input-limit conversion; first hardware pass may require changes.
- **Unresolved:** Tristar USB-online/negotiation and a verified A9 charging
  policy. Read-only telemetry is delivered; higher-current charging is not.
- **Unresolved:** loader BMac identity injection; BT RF calibration is not
  implemented even in the template. Preserve private records and do not
  claim calibrated RF performance.
- **Inferred:** J71t factory LSCI matches the template's supported record;
  unsupported records deliberately prevent an ALS probe.
- **Unimplemented:** A9 SIO/SPI/touch-clock/analog regulator port and N1
  touch runtime/firmware protocol; no touch device is opted in. Wi-Fi driver
  and SysCfg/NVMEM integration remain with the PCIe workstream.

## Device test 1 results (2026-10-22, kernel 744636325f11, no firmware staged)

Orchestrator run. dmesg: `docs/test-logs/2026-10-22-periph-r1-dmesg.txt`.
- **Bluetooth:** uart3 serdev up; `hci0: BCM: chip id 125`, `BCM4355C0`,
  `BCM (003.001.003) build 0000`. Then `firmware Patch file not found, tried:
  'brcm/BCM.apple,j71t.hcd', 'brcm/BCM.hcd'` (expected), and `HCI Read
  Transmit Power Level command is advertised, but not supported` (needs the
  quirk).
- **Battery (bq27540 over HDQ):** works. capacity 36, voltage 3.801 V, current
  -535 mA, temp 31.3 °C, charge_full 8036000, cycle_count 237, health Good.
  `charge_full_design` read returns EINVAL.
- **Charger:** `/sys/class/power_supply/charger` status=Discharging,
  type=Unknown, **while on USB from the Mac. The battery is really
  discharging**, so charging isn't enabled (Tristar/charger policy).
- **ALS (tsl2583/CT819):** blocked boot for 60 s. `Direct firmware load for
  apple/j71t-LSCI.bin failed`, then the sysfs fallback timed out (-110),
  probe failed. Fix: no sysfs fallback (`firmware_request_nowarn` /
  `request_firmware_direct`, or async probe), plus stage the LSCI file.
- USB gadget came up only after ~64 s because of the ALS stall.

Video-adjacent blocks, studied but not implemented:
[`ds-scaler-jpeg.md`](ds-scaler-jpeg.md) covers the `scaler0` MSR4 image
scaler/colour converter (`0x07900000`, IRQ 179, `dart-scaler`) and the `jpeg`
codec (`0x07b00000`, IRQ 175, `dart-jpeg`) — register maps, formats, the
APIODMA packet-stream model, and a ranked V4L2 m2m plan. Tooling:
`tools/ds-video/`.
