#!/usr/bin/env python3
"""Stage the charging policy in its owning kernel tree (no commits/device I/O)."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TREE = Path('/src/wt/periph')


def main():
    branch = subprocess.check_output(['git', '-C', str(TREE), 'branch',
                                      '--show-current'], text=True).strip()
    if branch != 'a9/periph':
        raise SystemExit(f'refusing to edit unexpected kernel branch {branch!r}')
    for name in ('apple_j71t_charger.c', 'apple_tristar_power.c'):
        shutil.copyfile(ROOT / 'scripts/periph/kernel' / name,
                        TREE / 'drivers/power/supply' / name)
    path = TREE / 'arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi'
    text = path.read_text()
    marker = '\t\tpower-supplies = <&periph_tristar>;'
    if 'apple,owner-input-limit-microamp' not in text:
        text = text.replace(marker, marker + '\n'
            '\t\tapple,usb-input-limit-max-microamp = <2400000>;\n'
            '\t\tapple,owner-input-limit-microamp = <2400000>;')
    path.write_text(text)
    path = TREE / 'Documentation/devicetree/bindings/power/supply/apple,d2365-charger.yaml'
    text = path.read_text()
    if 'apple,owner-input-limit-microamp:' not in text:
        text = text.replace('\nrequired:', '''
  apple,usb-input-limit-max-microamp:
    description: Board input ceiling, from the ADT usb-input-limit-max value.
    enum: [500000, 1000000, 2100000, 2400000]
    default: 500000

  apple,owner-input-limit-microamp:
    description: |
      Owner-authorized input ceiling for this development unit's USB host.
      Requires an enumerated host or a detected brick and valid CBAT codes;
      the board ceiling still applies. Omission leaves source-based policy.
    enum: [500000, 1000000, 2100000, 2400000]

required:''')
    path.write_text(text)


if __name__ == '__main__':
    main()
