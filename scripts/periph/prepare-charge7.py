#!/usr/bin/env python3
"""Stage charge7 only in /src/wt/charge on a9/charge; no device access."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path('/work')
TREE = Path('/src/wt/charge')


def main():
    assert subprocess.check_output(['git', '-C', str(TREE), 'branch',
                                    '--show-current'], text=True).strip() == 'a9/charge'
    subprocess.run(['git', '-C', str(TREE), 'merge-base', '--is-ancestor',
                    '2801e838fc', 'HEAD'], check=True)
    for name in ('apple_j71t_charger.c', 'apple_tristar_power.c',
                 'apple_tristar_auto.h', 'apple_tristar_source.h'):
        shutil.copyfile(ROOT / 'scripts/periph/kernel' / name,
                        TREE / 'drivers/power/supply' / name)
    shutil.copyfile(ROOT / 'scripts/periph/kernel/sysfs-apple-charge7',
                    TREE / 'Documentation/ABI/testing/sysfs-apple-charge7')
    periph = TREE / 'arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi'
    periph.write_text(periph.read_text().replace(
        '\t\tapple,owner-input-limit-microamp = <2400000>;\n', '').replace(
        '\t\tapple,usb-input-limit-max-microamp = <2400000>;\n', ''))
    common = TREE / 'arch/arm64/boot/dts/apple/s800x-ipad5.dtsi'
    text = common.read_text()
    if 'usb-input-limit-max-microamp' not in text:
        marker = '\n\t\trtc@500 {'
        assert text.count(marker) == 1
        text = text.replace(marker, '''
		/* ADT j71s/j71t/j72s/j72t: usb-input-limit-max = 0x960 mA. */
		charger@180 {
			reg = <0x180 0x1e>, <0xa00 0x0d>;
			reg-names = "status", "control";
			apple,usb-input-limit-max-microamp = <2400000>;
		};
''' + marker)
        common.write_text(text)
    binding = TREE / 'Documentation/devicetree/bindings/power/supply/apple,d2365-charger.yaml'
    text = binding.read_text()
    text = text.replace('J71t has an explicitly armed policy for a verified USB source, requiring',
                        'J71t uses the board ceiling for a USB host source, requiring')
    start = text.find('  apple,owner-input-limit-microamp:')
    if start >= 0:
        end = text.index('\nrequired:', start)
        text = text[:start] + text[end:]
    binding.write_text(text)
    # Small support diff, separate from the long-lived peripheral mirrors.
    patch = ROOT / 'scripts/periph/charge7-support.patch'
    if patch.exists():
        reverse = subprocess.run(['git', '-C', str(TREE), 'apply', '--reverse',
                                  '--check', str(patch)], capture_output=True)
        if reverse.returncode:
            subprocess.run(['git', '-C', str(TREE), 'apply', '--whitespace=error',
                            str(patch)], check=True)
    print('PASS staged charge7 sources on a9/charge')


if __name__ == '__main__':
    main()
