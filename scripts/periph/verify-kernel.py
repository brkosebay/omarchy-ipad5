#!/usr/bin/env python3
"""Run through build-kernel.sh --run periph; inspect built DTB and config."""
from pathlib import Path
import subprocess


def main():
    config = Path('.config').read_text().splitlines()
    fragment = Path('/work/scripts/periph/kernel.config').read_text().splitlines()
    for line in fragment:
        if line.startswith('CONFIG_'):
            assert line in config, f'missing kernel option: {line}'
    assert 'CONFIG_BATTERY_BQ27XXX_DT_UPDATES_NVM=y' not in config
    assert 'CONFIG_APPLE_BELLATRIX_POWER=y' not in config
    # ARM64 sets UART_NR to 18 regardless of the hidden UARTS=4 default.
    assert '#ifdef CONFIG_ARM64\n#define UART_NR\t\t\t18' in Path('drivers/tty/serial/samsung_tty.c').read_text()
    dtb = '/work/build/kernel-periph/dtbs/s8003-j71t.dtb'

    def prop(node, name, kind='x'):
        return subprocess.check_output(['fdtget', '-t', kind, dtb, node, name], text=True).split()

    def nums(node, name):
        return [int(n, 16) for n in prop(node, name)]

    uart3 = '/soc/serial@20a0cc000'
    uart5 = '/soc/serial@20a0d4000'
    als = '/soc/i2c@20a112000/light-sensor@39'
    pmic = '/soc/i2c@20a110000/pmic@3c'
    assert nums(uart3, 'reg') == [2, 0xa0cc000, 0, 0x4000]
    assert nums(uart3, 'interrupts') == [0, 195, 4]
    assert prop(uart3 + '/bluetooth', 'compatible', 's') == ['apple,j71t-bcm4355-bt']
    assert nums(uart3 + '/bluetooth', 'max-speed') == [3000000]
    assert nums(uart3 + '/bluetooth', 'device-wakeup-gpios')[1:] == [142, 0]
    assert nums(uart3 + '/bluetooth', 'shutdown-gpios')[1:] == [13, 0]
    assert nums(uart5, 'interrupts') == [0, 197, 4]
    assert prop(uart5 + '/onewire', 'compatible', 's') == ['ti,hdq-uart']
    assert prop(uart5 + '/onewire/fuel-gauge', 'compatible', 's') == ['apple,j71t-bq27540']
    assert nums(als, 'reg') == [0x39]
    assert nums(als, 'interrupts-extended')[1:] == [71, 8]
    assert prop(als, 'firmware-name', 's') == ['apple/j71t-LSCI.bin']
    assert prop(pmic + '/charger@180', 'compatible', 's') == ['apple,j71t-d2365-charger']
    assert nums(pmic, '#address-cells') == [1]
    assert nums(pmic, '#size-cells') == [0]
    assert nums(pmic + '/rtc@500', 'reg') == [0x500]
    assert nums(pmic + '/nvmem@5000', 'reg') == [0x5000]
    assert nums(pmic + '/nvmem@5000', 'size') == [0x28]
    assert prop(pmic + '/nvmem@5000', 'read-only') == []
    assert nums(pmic + '/charger@180', 'reg') == [0x180, 0xa00]
    tristar = '/soc/i2c@20a110000/power-controller@1a'
    usb = '/soc/bus@20c000000/usbdev@20c100000'
    assert prop(tristar, 'compatible', 's') == ['apple,j71t-cbtl1610-power']
    assert nums(tristar, 'reg') == [0x1a]
    assert nums(tristar, 'usb-controller') == nums(usb, 'phandle')
    assert nums(tristar, 'apple,pmic') == nums(pmic, 'phandle')
    assert prop(usb, 'apple,host-current-budget') == []
    assert nums(pmic + '/charger@180', 'power-supplies') == nums(tristar, 'phandle')
    assert nums(pmic + '/gpio', 'gpio-reserved-ranges') == [0, 13, 14, 10]
    # Exactly J71t is opted in. This guards against accidentally applying
    # this unit's GPIOs to Samsung or cellular variants via a shared include.
    for name in ('s8000-j71s', 's8000-j72s', 's8003-j72t'):
        result = subprocess.run(['fdtget', '/work/build/kernel-periph/dtbs/' + name + '.dtb', uart3, 'compatible'], capture_output=True)
        assert result.returncode != 0, f'unexpected peripheral opt-in on {name}'
    symbols = subprocess.check_output(['llvm-nm', '--defined-only', 'vmlinux'], text=True)
    for name in ('bcm_setup', 'w1_uart_hdq_read_block', 'apple_d2365_charger_probe',
                 'j71t_probe', 'j71t_policy_work', 'apple_tristar_probe',
                 'apple_tristar_poll', 'usb_gadget_get_current_budget',
                 'usb_gadget_register_current_notifier', 'tsl2583_probe', 'd2365_gpio_probe'):
        assert name in symbols, f'driver not linked: {name}'
    assert not Path('/work/build/kernel-periph/DIRTY').read_text().strip(), 'exported build was dirty'
    for name in ('apple_j71t_charger.c', 'apple_tristar_power.c'):
        assert Path('drivers/power/supply', name).read_bytes() == Path(
            '/work/scripts/periph/kernel', name).read_bytes(), f'tested template differs: {name}'
    print('PASS: source-aware charging linked, address-only PMIC DT, safe config and J71t isolation')


if __name__ == '__main__':
    main()
