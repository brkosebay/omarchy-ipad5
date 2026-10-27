# Round 3 peripherals delivery

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. The implementation agent never contacted the iPad and made no
linux-a9 host-repository commits. Touch is owned by the dedicated touch
agent; `touch-recon.md` was left untouched after the scope transfer.

## Changes and evidence

- **ALS, verified on device by the orchestrator:** integration kernel with
  `79aef2ef96` loaded this unit's LSCI v2 and found the light sensor at
  0.75 s. Missing LSCI uses direct firmware loading and cannot stall boot
  in the sysfs fallback. Lux response across light levels remains to test.
- **Bluetooth, verified in source:** J71t already selects
  `HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER`. The advertised-but-unsupported
  warning is emitted because this quirk is active; it does not mean the
  command is being sent. The quirk is retained/documented. J71t HCD lookup
  now also uses direct loading, avoiding its observed second 60-second
  helper wait. The staged C0 FearA USI image is **inferred** from this
  unit's verified combo-module vendor; device patch revision remains to test.
- **Battery:** the unsupported cache-only charge_full_design property is
  hidden. Valid future monitored-battery design data remains supported by
  the power-supply core.
- **Charging, verified statically/offline:** dedicated J71t D2365 policy,
  private CBAT conversion, read-only Tristar VBUS supplier and exact iOS
  STOP-separated I2C framing. Explicit owner-authorized input request
  2400000 uA programs calibrated `0xba` input and a hardware-capped 2500 mA
  battery limit. Both supplies expose online; actual limits are readable.
  Temperature/voltage/status/source guards, checked writes and restoration
  apply throughout. See [charging-recon.md](charging-recon.md). **Pending
  device validation:** positive battery current near the iOS +1.7 A reference.
  Automatic accessory current negotiation remains unresolved; this iteration
  uses an explicit request only for the already measured Mac port.
- **BlueZ userspace:** static AArch64/musl BlueZ 5.85 btmgmt is packaged with
  its license and optional private RAM-only Bluetooth identity input. It needs
  neither a daemon nor D-Bus. No bluetoothctl/profile daemon is packaged.
- **Initramfs correction:** one deterministic gzip stream now carries both
  the original HoolockRD cpio and the firmware/tools cpio. The earlier two
  gzip members caused m1n1 to replace the base initrd with firmware-only
  contents, losing /init. This explained the integration VFS panic. The new
  regression check reproduces/rejects that format and confirms /init survives.

## Delivery and checks

Kernel, DTBs and provenance are in `build/kernel-periph/`; current immutable
hashes and the exact runnable procedure are in [test-plan.md](test-plan.md).
The 42-patch series in `patches/periph/` replays from baseline `6831bc701`.
New commits 40–42 cover bindings, drivers and J71t-only DT wiring; no other
board opts into the new policy. Private Apple binaries/calibration remain
under build/. CBAT is newly staged from this unit's resolved ADT record.

Validation includes the full Docker kernel/104-DTB build, scoped DT schema
checks, clean patch replay, linked-driver/DT isolation checks, 13 firmware
parser/selection tests, exact checks of 12 peripherals firmware files, 56
charging fault/guard cases under ASan/UBSan, and two initramfs regression
cases. Additional firmware staged by another workstream is preserved and
reported outside the peripherals verifier's claim. Read the build/test logs
under build/periph for the precise scope and any pre-existing schema output.

Build payload on the host:

```sh
TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz
```

After boot, the measured Mac-port charging request is:

```sh
printf '2400000\n' > /sys/class/power_supply/charger/input_current_limit
```

The full plan first captures handoff values and then verifies 2400000 input,
2500000 battery-current limit, online=1, charging-current sign and restoration.
The implementation agent did not run these device commands.
