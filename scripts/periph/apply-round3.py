#!/usr/bin/env python3
"""Apply the device-test-1 fixes in /src/wt/periph, without hardware access.

Run with scripts/build-kernel.sh --run periph. Commits only this worktree.
"""
from pathlib import Path
import subprocess
import sys
import textwrap


def change(path, old, new):
    p = Path(path)
    text = p.read_text()
    if new in text:
        return False
    if text.count(old) != 1:
        raise RuntimeError(f'expected exactly one source match: {path}')
    p.write_text(text.replace(old, new, 1))
    return True


def commit(paths, subject, body):
    subprocess.run(['git', 'diff', '--check'], check=True)
    subprocess.run(['git', 'add', '--', *paths], check=True)
    body = '\n\n'.join(textwrap.fill(p, 72) for p in body.split('\n\n'))
    subprocess.run(['git', 'commit', '-m', subject, '-m', body], check=True)


def restyle_head():
    """Restyle only this round's three unpublished commits during rebase."""
    if Path.cwd() != Path('/src/wt/periph'):
        raise RuntimeError('run only in /src/wt/periph')
    message = subprocess.check_output(['git', 'show', '-s', '--format=%s%n%b'], text=True)
    subject, body = message.split('\n', 1)
    if subject not in (
        'iio: light: tsl2583: never wait for sysfs LSCI fallback',
        'Bluetooth: btbcm: avoid J71t sysfs firmware fallback',
        'power: supply: bq27xxx: hide unavailable cached design charge',
    ):
        raise RuntimeError('refusing to restyle another commit')
    bt = Path('drivers/bluetooth/btbcm.c')
    text = bt.read_text()
    # Keep the short argument list on one line; no continuation alignment.
    text = text.replace('request_firmware_direct(&fw, fw_name[i],\n\t\t\t\t\t      &hdev->dev);',
                        'request_firmware_direct(&fw, fw_name[i], &hdev->dev);')
    text = text.replace('firmware_request_nowarn(&fw, fw_name[i],\n\t\t\t\t\t       &hdev->dev);',
                        'firmware_request_nowarn(&fw, fw_name[i], &hdev->dev);')
    if text != bt.read_text():
        bt.write_text(text)
        subprocess.run(['git', 'add', '--', str(bt)], check=True)
    body = '\n\n'.join(textwrap.fill(p, 72) for p in body.strip().split('\n\n'))
    subprocess.run(['git', 'commit', '--amend', '-m', subject, '-m', body], check=True)


def main():
    if Path.cwd() != Path('/src/wt/periph'):
        raise RuntimeError('run only in /src/wt/periph')
    if subprocess.check_output(['git', 'status', '--porcelain']):
        raise RuntimeError('refusing to edit a dirty worktree')
    als = 'drivers/iio/light/tsl2583.c'
    if change(als, '\t\tret = request_firmware(&fw, fw_name, dev);',
              '\t\t/* Missing calibration must not wait for a userspace helper. */\n'
              '\t\tret = request_firmware_direct(&fw, fw_name, dev);'):
        commit([als], 'iio: light: tsl2583: never wait for sysfs LSCI fallback',
               'J71t device test 1 stalled the boot probe for over 62 seconds when '
               'the private LSCI file was absent. request_firmware_direct sets '
               'FW_OPT_NOFALLBACK_SYSFS even with forced fallback enabled. '
               'Keep the calibration mandatory and fail promptly; retain NVMEM preference.')
    bt = 'drivers/bluetooth/btbcm.c'
    bcm = 'drivers/bluetooth/hci_bcm.c'
    altered = change(bt, '\t\terr = firmware_request_nowarn(&fw, fw_name[i], &hdev->dev);',
                     '\t\t/* J71t RAM boots have no firmware helper; fail each name fast. */\n'
                     '\t\tif (board_name && !strcmp(board_name, "apple,j71t"))\n'
                     '\t\t\terr = request_firmware_direct(&fw, fw_name[i], &hdev->dev);\n'
                     '\t\telse\n'
                     '\t\t\terr = firmware_request_nowarn(&fw, fw_name[i], &hdev->dev);')
    altered |= change(bcm, '\t.broken_read_transmit_power = true,\n\t.fixup_le_adv_report_evt_type = true,',
                      '\t/* Also required by J71t BCM4355C0; the core logs active quirks. */\n'
                      '\t.broken_read_transmit_power = true,\n\t.fixup_le_adv_report_evt_type = true,')
    if altered:
        commit([bt, bcm], 'Bluetooth: btbcm: avoid J71t sysfs firmware fallback',
               'The same boot log has another roughly 60-second wait before the '
               'missing HCD report: firmware_request_nowarn suppresses warnings '
               'but still permits forced sysfs fallback. Use direct requests only '
               'for the J71t board, retaining generic controller behavior.\n\n'
               'The transmit-power warning is emitted by hci_broken_table because '
               'HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER is already set. Preserve that '
               'quirk and document the C0 board sharing the match data; no duplicate '
               'quirk or log suppression is needed.')
    battery = 'drivers/power/supply/bq27xxx_battery.c'
    text = Path(battery).read_text()
    start = text.index('static const enum power_supply_property bq27545_cache_props[]')
    end = text.index('\n};', start)
    old = text[start:end]
    if '\tPOWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,\n' in old:
        new = old.replace('\tPOWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,\n', '')
        change(battery, old, new)
        change(battery,
               ' * initiate bus transactions. Design charge may be supplied as static board\n'
               ' * data through monitored-battery.',
               ' * initiate bus transactions. The power-supply core exposes design charge\n'
               ' * only when valid static monitored-battery data is available.')
        commit([battery], 'power: supply: bq27xxx: hide unavailable cached design charge',
               'The cache-only BQ27545 profile does not read design capacity and '
               'J71t has no monitored-battery design-charge data. Advertising the '
               'property unconditionally exposes a sysfs file that always returns '
               'EINVAL. Remove it from the cache property list. The power-supply '
               'core can still expose it when valid battery_info is supplied; '
               'do not invent a capacity or add gauge control/data-flash accesses.')


if __name__ == '__main__':
    if sys.argv[1:] == ['--restyle-head']:
        restyle_head()
    else:
        main()
