#!/usr/bin/env python3
"""Check the compiled IRQ Smart Cover DTB and its polled fallback, offline."""
import argparse
from pathlib import Path
import re
import subprocess


def prop(dtb, node, name, kind='x'):
    return subprocess.check_output(
        ['fdtget', '-t', kind, str(dtb), node, name], text=True).strip()


def has(dtb, node, name):
    return subprocess.run(['fdtget', str(dtb), node, name],
                          capture_output=True).returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    irq = args.directory / 's8003-j71t-smart-cover.dtb'
    fallback = args.directory / 's8003-j71t.dtb'
    pmic = '/soc/i2c@20a110000/pmic@3c'
    gpio = pmic + '/gpio'
    # Resolve the parent by phandle instead of depending on its path name.
    dts = subprocess.check_output(['dtc', '-q', '-I', 'dtb', '-O', 'dts', str(irq)],
                                  text=True)
    assert prop(irq, pmic, 'interrupts') == '4c 8'
    assert prop(irq, pmic, '#interrupt-cells') == '1'
    assert has(irq, pmic, 'interrupt-controller') and has(irq, pmic, 'wakeup-source')
    parent = prop(irq, pmic, 'interrupt-parent')
    match = re.search(r'(?m)^\s*(pinctrl|gpio)@20f100000 \{(.*?)^\s*\};', dts, re.S)
    assert match and re.search(r'phandle = <0x0*' + parent + r'>;', match.group(2))
    assert has(irq, gpio, 'apple,enable-hall-irqs')
    assert not has(fallback, gpio, 'apple,enable-hall-irqs')
    pmic_handle = prop(irq, pmic, 'phandle')
    for name, child, event, line, code in (
        ('smart-cover', 'lid-switch', '30', '0', '0'),
        ('smart-cover-aux', 'cover-switch', '3a', 'a', '10'),
    ):
        node = '/' + name
        button = node + '/' + child
        assert prop(irq, node, 'compatible', 's') == 'gpio-keys'
        assert not has(irq, node, 'poll-interval')
        assert prop(irq, button, 'interrupts-extended') == pmic_handle + ' ' + event
        assert prop(irq, button, 'gpios').split()[1:] == [line, '1']
        assert prop(irq, button, 'linux,input-type') == '5'
        assert prop(irq, button, 'linux,code') == code
        assert prop(irq, button, 'debounce-interval') == 'e'
        assert has(irq, button, 'wakeup-source')
        assert prop(fallback, node, 'compatible', 's') == 'gpio-keys-polled'
        assert prop(fallback, node, 'poll-interval', 'u') == '50'
    print('PASS DT: AP GPIO 76 level-low; PMU events 0x30/0x3a; SW_LID/aux wake; 14 ms')
    print('PASS fallback DT: both halls polled; no PMU event opt-in')


if __name__ == '__main__':
    main()
