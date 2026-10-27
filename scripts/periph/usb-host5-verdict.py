#!/usr/bin/env python3
"""usb-host5 pass/fail from the iPad logger run (pulled) and the Mac watch log.

  usb-host5-verdict.py DIR [--watch-log FILE]
  usb-host5-verdict.py --self-test

Criteria (docs/periph/usb-host5-test-plan.md):
  host_role      policy reached host with the adapter ID 11 f0
  usb_ctl        USB_CTL +00 = 00000003 in host mode (the iOS bit 1 landed)
  hub_hs         1a40:0101 hub enumerated, at 480M
  drive_ro       a USB partition mounted read-only (then unmounted)
  charging_host  battery Charging with current_now > 0 while host (accessory), or a
                 Full / Not charging battery at >= -100 mA (the iPad runs from the
                 adapter; the PMIC has ended the charge)
  input_2100     charger input limit >= 2000000 uA while host (accessory); the
                 calibrated 2100 mA code reads back as 2075000
  no_suspend     charger never offline while VBUS was present across the switch
  input_held     once host mode reached >= 2000000 uA input, it never fell back to
                 <= 100000 uA (the 100 mA handoff) while VBUS stayed present
  detach_device  after the unplug: role device, phase empty/other
  gadget_mac     Mac tether SSH answered the same boot after the unplug
"""
import argparse
import re
import sys
import tempfile
from pathlib import Path


def kv_state(text):
    out = {}
    for line in text.splitlines():
        if ': ' in line:
            key, val = line.split(': ', 1)
            out[key.strip()] = val.strip()
    return out


def num(pattern, text):
    m = re.search(pattern, text)
    return int(m.group(1)) if m else None


def snapshots(run):
    for snap in sorted(p for p in run.iterdir() if p.is_dir()):
        state_file = snap / 'lightning_state.txt'
        if not state_file.exists():
            continue
        def read(name):
            f = snap / name
            return f.read_text(errors='replace') if f.exists() else ''
        yield snap.name, kv_state(read('lightning_state.txt')), read('lightning_state.txt'), \
            read('regs.txt'), read('lsusb.txt')


def evaluate(run, watch_log):
    res = {k: False for k in ('host_role', 'usb_ctl', 'hub_hs', 'drive_ro', 'charging_host',
                              'input_2100', 'no_suspend', 'input_held', 'detach_device',
                              'gadget_mac')}
    notes = []
    was_host = False
    suspend_bad = []
    reached_2100 = False
    dropped = []
    for name, st, raw, regs, lsusb in snapshots(run):
        role = st.get('role', '')
        host = role.startswith('host') and '(accessory)' in role
        vbus = 'vbus 1' in st.get('status_0c', '')
        chg_online = num(r'charger: online=(-?\d+)', raw)
        if host and '11 f0' in st.get('digital_id', ''):
            res['host_role'] = True
            was_host = True
        if host and re.search(r'usb_ctl: \+00=00000003', regs):
            res['usb_ctl'] = True
        if host and '1a40:0101' in lsusb and re.search(r'Class=Hub.*480M', lsusb):
            res['hub_hs'] = True
        cur = num(r'current_now_ua=(-?\d+)', raw)
        limit = num(r'input_current_limit_ua=(-?\d+)', raw)
        bat = re.search(r'battery: status=([A-Za-z ]+?) current', raw)
        bat = bat.group(1) if bat else ''
        if host and cur is not None and (
                (bat == 'Charging' and cur > 0) or
                (bat in ('Full', 'Not charging') and cur >= -100000 and
                 (limit or 0) >= 2000000)):
            res['charging_host'] = True
        if host and (limit or 0) >= 2000000:
            res['input_2100'] = True
            reached_2100 = True
        elif host and vbus and reached_2100 and limit is not None and limit <= 100000:
            dropped.append(name)
        if st.get('phase') in ('handshake', 'enter-host', 'host') and vbus and chg_online == 0:
            suspend_bad.append(name)
        if was_host and role.startswith('device') and st.get('phase') in ('empty', 'other'):
            res['detach_device'] = True
    res['no_suspend'] = res['host_role'] and not suspend_bad
    res['input_held'] = reached_2100 and not dropped
    if dropped:
        notes.append('input fell back to the 100 mA handoff in: ' + ', '.join(dropped))
    if suspend_bad:
        notes.append('charger offline with VBUS in: ' + ', '.join(suspend_bad))
    for f in sorted(run.glob('mount-*.txt')):
        text = f.read_text(errors='replace')
        if 'mounted read-only' in text and re.search(r'\bro\b', text) and 'unmounted' in text:
            res['drive_ro'] = True
    if watch_log and watch_log.exists():
        res['gadget_mac'] = 'GADGET OK' in watch_log.read_text(errors='replace')
    else:
        notes.append('no watch log: gadget_mac needs the Mac-side watch')
    return res, notes


def self_test():
    host_state = """auto_host: 1
phase: host
role: host (accessory)
status_0c: 0xa3 (vbus 1, present 1, orientation 0)
digital_id: 11 f0 00 00 00 00 (camera adapter)
power_handshake: ok (0, tries 1)
supply: online 1, type ACA, current_max_ua 2100000, error 0
charger: online=1 status=Charging input_current_limit_ua=2075000
battery: status=Charging current_now_ua=812000 voltage_now_uv=4249000 capacity=55
"""
    regs = 'usb_ctl: +00=00000003 +04=00000000 (host select 1, phy-on bit1 1 set by switch 1)\n'
    lsusb = ('Bus 001 Device 002: ID 1a40:0101 Terminus Technology Inc. Hub\n'
             '/:  Bus 001.Port 001: Dev 001, Class=root_hub, Driver=ehci-platform/1p, 480M\n'
             '    |__ Port 001: Dev 002, If 0, Class=Hub, Driver=hub/4p, 480M\n')
    after = host_state.replace('phase: host', 'phase: empty').replace(
        'role: host (accessory)', 'role: device')
    with tempfile.TemporaryDirectory() as tmp:
        run = Path(tmp)
        for i, (st, rg, ls) in enumerate(((host_state, regs, lsusb), (after, '', ''))):
            d = run / f'{i:03d}-x'
            d.mkdir()
            (d / 'lightning_state.txt').write_text(st)
            (d / 'regs.txt').write_text(rg)
            (d / 'lsusb.txt').write_text(ls)
        (run / 'mount-sda1.txt').write_text(
            'mounted read-only at /run/x:\n/dev/sda1 exfat ro,nosuid\nunmounted\n')
        wl = run / 'watch.log'
        wl.write_text('12:00:00 MAC: GADGET OK - tether SSH answers, same boot x\n')
        res, notes = evaluate(run, wl)
        assert all(res.values()), (res, notes)
        # Mutations: each criterion must be able to fail.
        (run / '000-x' / 'regs.txt').write_text(regs.replace('00000003', '00000001'))
        assert not evaluate(run, wl)[0]['usb_ctl']
        (run / '000-x' / 'lightning_state.txt').write_text(
            host_state.replace('charger: online=1', 'charger: online=0'))
        assert not evaluate(run, wl)[0]['no_suspend']
        (run / '000-x' / 'lightning_state.txt').write_text(
            host_state.replace('current_now_ua=812000', 'current_now_ua=-300000'))
        assert not evaluate(run, wl)[0]['charging_host']
        d2 = run / '001-y'
        d2.mkdir()
        (d2 / 'lightning_state.txt').write_text(
            host_state.replace('input_current_limit_ua=2075000', 'input_current_limit_ua=100000'))
        assert not evaluate(run, wl)[0]['input_held']
        (d2 / 'lightning_state.txt').unlink()
        d2.rmdir()
        (run / '000-x' / 'lightning_state.txt').write_text(
            host_state.replace('status=Charging current_now_ua=812000',
                               'status=Full current_now_ua=-20000'))
        assert evaluate(run, wl)[0]['charging_host'], 'full battery on adapter power'
        (run / '000-x' / 'lightning_state.txt').write_text(
            host_state.replace('input_current_limit_ua=2075000',
                               'input_current_limit_ua=100000'))
        assert not evaluate(run, wl)[0]['input_2100']
        (run / '000-x' / 'lsusb.txt').write_text(lsusb.replace('480M', '12M'))
        assert not evaluate(run, wl)[0]['hub_hs']
        (run / 'mount-sda1.txt').write_text('read-only mount FAILED\n')
        assert not evaluate(run, wl)[0]['drive_ro']
        wl.write_text('nothing\n')
        assert not evaluate(run, wl)[0]['gadget_mac']
    print('PASS usb-host5 verdict self-test (10 criteria, 8 mutations, full-battery case)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', nargs='?')
    ap.add_argument('--watch-log')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.dir:
        ap.error('DIR required')
    res, notes = evaluate(Path(args.dir), Path(args.watch_log) if args.watch_log else None)
    for key, ok in res.items():
        print(f'{"PASS" if ok else "FAIL"} {key}')
    for n in notes:
        print(f'note: {n}')
    print('OVERALL', 'PASS' if all(res.values()) else 'FAIL')
    return 0 if all(res.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
