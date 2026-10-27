#!/usr/bin/env python3
"""Commit only the Round 3 charging files, inside the periph kernel worktree."""
from pathlib import Path
import subprocess
import textwrap

if Path.cwd() != Path('/src/wt/periph'):
    raise ValueError('run only in /src/wt/periph')
groups = [
    ('dt-bindings: power: describe J71t Tristar supplier and charging',
     'Describe the J71t-only CBTL1610 presence supplier and require it for '
     'the explicitly armed D2365 charger policy.',
     ['Documentation/devicetree/bindings/power/supply/apple,d2365-charger.yaml',
      'Documentation/devicetree/bindings/power/supply/apple,j71t-cbtl1610-power.yaml']),
    ('power: supply: add explicitly armed J71t charging with rollback',
     'Reconstruct D2365 input-current calibration and controls from the iOS '
     'kernelcache. Require private CBAT, strict Tristar VBUS presence, safe '
     'battery telemetry and an explicit 2400000-uA request for the owner-verified '
     'Mac source. Program calibrated input and capped 2500-mA battery limits '
     'while input is suspended; preserve unrelated bits and verify every write. '
     'Restore handoff settings on disconnect, faults, suspend and removal. '
     'No automatic source-current entitlement is inferred from VBUS.\n\n'
     'Expose live ONLINE, input-limit and battery-current-limit readback. '
     'Use the verified STOP-separated Tristar byte-read framing without '
     'writing IC configuration or consuming interrupts. J172 policy is unchanged.\n\n'
     'Validation: actual calibration and transaction functions pass 56 '
     'fault/guard cases under ASan/UBSan, including partial writes, mismatched '
     'readback and rollback recovery. Hardware charging validation is pending.',
     ['drivers/power/supply/Kconfig', 'drivers/power/supply/Makefile',
      'drivers/power/supply/apple_d2365_charger.c',
      'drivers/power/supply/apple_j71t_charger.c',
      'drivers/power/supply/apple_tristar_power.c']),
    ('arm64: dts: apple: connect J71t charger to Tristar VBUS supplier',
     'Describe CBTL1610 at the ADT I2C0 address and connect it to the '
     'J71t-only charger. The source driver is read-only; a measured Mac '
     'current budget still requires explicit userspace authorization.',
     ['arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi']),
]
for title, body, paths in groups:
    dirty = subprocess.check_output(['git', 'status', '--porcelain', '--', *paths], text=True)
    if not dirty:
        continue
    subprocess.run(['git', 'add', '--', *paths], check=True)
    message = '\n\n'.join(textwrap.fill(p, 72) for p in body.split('\n\n'))
    subprocess.run(['git', 'commit', '-m', title, '-m', message, '--', *paths], check=True)
