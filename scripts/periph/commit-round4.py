#!/usr/bin/env python3
"""Commit reviewed Round 4 changes in the isolated kernel tree only."""
from pathlib import Path
import subprocess
import textwrap

if Path.cwd() != Path('/src/wt/periph'):
    raise SystemExit('run only in /src/wt/periph')
if subprocess.check_output(['git', 'branch', '--show-current'], text=True).strip() != 'a9/periph':
    raise SystemExit('wrong branch')
groups = [
    ('dt-bindings: mfd: apple: describe address-only PMIC children',
     'Permit a non-MMIO register bus with zero size cells. Preserve the legacy format and document a separate byte count for address-only NVMEM ranges.',
     ['Documentation/devicetree/bindings/mfd/apple,pmic.yaml', 'Documentation/devicetree/bindings/nvmem/apple,pmic-nvmem.yaml']),
    ('nvmem: simple-mfd: accept address-only PMIC register ranges',
     'Read the explicit positive size when the parent has zero size cells, while retaining legacy base/size tuples. This lets J71t reject MMIO translation without losing RTC-offset NVMEM reads.',
     ['drivers/nvmem/simple-mfd-nvmem.c']),
    ('dt-bindings: power: apple: describe J71t USB source allowances',
     'Connect Tristar to its D2365 ADC and USB device controller. Opt in the J71t gadget current protocol explicitly; VBUS alone is not a high-current authorization.',
     ['Documentation/devicetree/bindings/power/supply/apple,j71t-cbtl1610-power.yaml', 'Documentation/devicetree/bindings/usb/dwc2.yaml']),
    ('usb: gadget: report bounded Apple host current grants',
     'Handle the verified zero-data vendor request only on opted-in boards. Cache standard and signed extra allowances separately from optional PHY programming, publish sleepable queries and atomic notifications, and revoke elevated grants on reset, disconnect, suspend and transfer failure. Cap this platform at 2400 mA.',
     ['drivers/usb/gadget/composite.c', 'drivers/usb/gadget/udc/core.c', 'include/linux/usb/gadget.h']),
    ('Bluetooth: btbcm: install J71t identity before final validation',
     'Prefer valid device-tree identity or directly load the private six-byte BMac record. Program controller RAM after Patchram reset and require exact address readback before accepting setup. No device identity or persistent write is embedded in the kernel.',
     ['drivers/bluetooth/btbcm.c', 'drivers/bluetooth/hci_bcm.c']),
    ('Bluetooth: log the established TX-power workaround at debug level',
     'Retain HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER and its unsupported-command guard. Only lower the diagnostic level for that established workaround; retain all other broken-command warnings.',
     ['net/bluetooth/hci_sync.c']),
    ('power: supply: apple: automate guarded J71t source-current policy',
     'Use observed USB host grants or verified Apple resistor signatures, with conservative standard USB allowances and private CBAT calibration. Add bounded D2365 completion polling and exact Tristar route restoration. Retain battery, VBUS, hardware-cap, timeout, readback and rollback guards; fault-latch uncertain hardware errors. Export source type and system scope for desktop consumers.',
     ['drivers/power/supply/Kconfig', 'drivers/power/supply/apple_j71t_charger.c', 'drivers/power/supply/apple_tristar_power.c']),
    ('arm64: dts: apple: enable J71t source budgets and fix PMIC cells',
     'Keep the changes in the board peripheral include. Describe internal PMIC offsets with zero size cells and consistent child addresses, preserve NVMEM length and make it read-only. Wire Tristar to D2365 and the opted-in UDC for automatic guarded source budgets.',
     ['arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi']),
]
expected = {p for _, _, paths in groups for p in paths}
changed = set(subprocess.check_output(['git', 'diff', '--name-only', 'HEAD'], text=True).splitlines())
if changed != expected:
    raise SystemExit(f'changed paths differ from reviewed set: {changed ^ expected}')
subprocess.run(['git', 'diff', '--check'], check=True)
for subject, body, paths in groups:
    subprocess.run(['git', 'add', '--', *paths], check=True)
    subprocess.run(['git', 'commit', '-m', subject, '-m', textwrap.fill(body, width=72), '--', *paths], check=True)
if subprocess.check_output(['git', 'status', '--porcelain'], text=True).strip():
    raise SystemExit('unexpected dirty tree after commits')
