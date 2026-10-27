#!/usr/bin/env python3
"""Host-only test of scripts/showcase/bring-up.sh: OMARCHY_SSH points at a fake
ssh that answers from a scenario, so the parsing and the PASS/FAIL decisions run
without the iPad. Needs build/showcase (scripts/showcase/package.py) first."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
ENV = dict(line.split('=', 1) for line in (ROOT / 'build/showcase/entries.env').read_text().splitlines())
STOCK = (300000000, 396000000, 600000000, 912000000, 1200000000, 1512000000, 1800000000)

FAKE = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
d = Path(os.environ['FAKE_DIR'])
s = json.loads((d / 'scenario.json').read_text())
cmd = ' '.join(sys.argv[1:])
stdin = sys.stdin.read() if cmd == 'bash -s' or 'tee ' in cmd or 'cat >' in cmd else ''
with open(d / 'commands.log', 'a') as log:
    log.write(json.dumps([cmd, stdin[:2000]]) + '\n')
def out(text, code=0):
    sys.stdout.write(text)
    sys.exit(code)
if cmd == 'true':
    out('')
if 'cat /proc/cmdline' in stdin:
    out(s['identity'])
if 'apple-a9-aop-active/state' in stdin:
    out(s['aop'])
if 'HasAccelerometer' in stdin:
    out(s['rotate'])
if 'ipad-als-brightness' in stdin:
    out(s['als'])
if 'cold_release' in cmd:
    out(s['cold_release'] + '\n')
if 'cd /lib/firmware/apple/aop' in cmd:
    out(s['firmware'])
if 'sha256sum /root/a9-showcase/' in cmd:
    name = cmd.split('/root/a9-showcase/')[1].split()[0]
    out(s['hold_sums'].get(name, '') + '  x\n')
if 'is-active linux-a9-oc-hold' in cmd:
    out(s['hold_active'] + '\n')
if 'a9-oc-hold.py status' in cmd:
    out(json.dumps(s['status']) + '\n')
if 'a9-oc-hold.py preflight' in cmd:
    out(json.dumps(s['preflight']) + '\n', 0 if s['preflight']['ok'] else 1)
if 'a9-oc-hold.py load-test' in cmd:
    out(json.dumps(s['load']) + '\n')
if 'systemd-run' in cmd:
    s['status']['held'] = True
    (d / 'scenario.json').write_text(json.dumps(s))
    out('Running as unit: linux-a9-oc-hold.service\n')
if 'f=~/.config/fastfetch' in cmd:
    out(s['fastfetch_remote'])
if 'sha256sum ~/.config/fastfetch' in cmd:
    out(s['fastfetch_sum'] + '  x\n')
if 'fastfetch --logo none --pipe' in cmd:
    out(s['fastfetch'])
out('')
'''


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def scenario():
    cmdline = (Path(ENV['SHOWCASE_DIR']) / 'cmdline').read_text().strip()
    opps = ''.join(f'opp opp0{i + 1} {hz} {i + 1} 0\n' for i, hz in enumerate(STOCK)) + 'opp opp14 2160000000 14 1\n'
    state = ('format=1 phase=streaming result=streaming errno=0 path=cold\n'
             'cold_run=2 system_ready=1 iop_crashed=0 accel_endpoint=0x21\n'
             'accel cal_sent=3 arot_sent=0 rate_set=1 reports={} report_errors=0 iio=1 cal_unsupported=1 cal_state=2\n')
    guard = dict(armed='1', latched='0', reason='armed', limit_khz='2160000', configured='4000', dirty='0',
                 delta_uv='0', profile='sustained', passive_millic='90000')
    policy = dict(cpuinfo_max_freq='2160000', scaling_max_freq='2160000', boost='1', scaling_governor='schedutil')
    sums = (Path(ENV['SHOWCASE_ROOT_FIRMWARE']) / 'SHA256SUMS').read_text()
    config = ROOT / 'scripts/omarchy/rootfs/tablet/fastfetch.jsonc'
    return dict(
        identity=f'{cmdline}\n{ENV["SHOWCASE_RELEASE"]}\nboot-1234\n{opps}',
        aop=('== state1\n' + state.format(611) + '== state2\n' + state.format(641) +
             '== dmesg\n[   45.8] apple-a9-aop-active 210800000.aop: SPI0 reset decision=skip reason=fresh-instance auto_boot=0\n'
             '== iio\ndev /sys/bus/iio/devices/iio:device1 287 -6 -64748\n'),
        rotate='proxy=active\naccelerometer=b true\nunit=active\n'
               'status={"source": "proxy", "sensor": "right-up", "acted": "right-up", "orientation": "landscape", "locked": false}\n',
        als='sensor=/sys/bus/iio/devices/iio:device0/in_illuminance_input\nenabled=on\nunit=active\nstatus=on: 120 lux -> 48 %\n',
        cold_release='defer=1 triggered=1', firmware=sums,
        hold_sums={p.name: sha(p) for p in (ROOT / 'scripts/power/ladder.py', ROOT / 'scripts/power/a9_thermal.py',
                                            ROOT / 'scripts/showcase/device/a9-oc-hold.py')},
        hold_active='inactive', status=dict(held=False, guard=dict(guard, armed='0', latched='1', reason='user-revert', configured='0', limit_khz='1800000'), policy=policy),
        preflight=dict(ok=True, checks=[]),
        load=dict(event='load-test', ok=True, target_khz=2160000, samples=32, at_target=31, max_khz=2160000,
                  median_khz=2160000, hottest_millic=71500, error=None),
        fastfetch_remote=f'{sha(config)}\n// linux-a9: fastfetch\n', fastfetch_sum=sha(config),
        fastfetch=' Device: iPad 5 (Apple A9)\n│ ├: Apple A9 (S8003), 2x Twister @ 2160 MHz\n'
                  '└ └󰖩: Wi-Fi 5 GHz connected · Bluetooth none connected\n',
        _guard=guard)


class BringUpTests(unittest.TestCase):
    def run_script(self, change=None, args=()):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = Path(tmp.name)
        s = scenario()
        if change:
            change(s)
        if s['status']['held']:
            s['status']['guard'] = s['_guard']
        (d / 'scenario.json').write_text(json.dumps(s))
        (d / 'ssh').write_text(FAKE.replace('#!/usr/bin/env python3', f'#!{sys.executable}', 1))
        (d / 'ssh').chmod(0o755)
        env = dict(os.environ, OMARCHY_SSH=str(d / 'ssh'), FAKE_DIR=str(d), SHOWCASE_RUN_DIR=str(d / 'run'),
                   SHOWCASE_SSH_WAIT='5')
        done = subprocess.run(['bash', str(ROOT / 'scripts/showcase/bring-up.sh'), *args], env=env,
                              capture_output=True, text=True, timeout=120)
        self.commands = [json.loads(l) for l in (d / 'commands.log').read_text().splitlines()]
        summary = (d / 'run/summary.txt').read_text()
        for cmd, stdin in self.commands:
            text = cmd + stdin
            for word in ('upload-entry', 'kexec', '/boot/linux-a9', 'reboot', 'nvram'):
                self.assertNotIn(word, text)
        return done.returncode, summary

    def line(self, summary, name):
        return next(l for l in summary.splitlines() if l[5:].startswith(name))

    def test_everything_passes_and_starts_the_hold(self):
        code, summary = self.run_script()
        self.assertEqual(code, 0, summary)
        self.assertIn('RESULT: PASS', summary)
        for name in ('entry', 'firmware', 'aop', 'rotate', 'cpu', 'als', 'fastfetch'):
            self.assertTrue(self.line(summary, name).startswith('PASS'), self.line(summary, name))
        self.assertIn('reports 611->641', summary)
        self.assertIn('31/32 samples at 2160 MHz', summary)
        self.assertIn('@ 2160 MHz', summary)
        run = [c for c, _ in self.commands if 'systemd-run' in c]
        self.assertEqual(len(run), 1)
        self.assertIn('--unit=linux-a9-oc-hold', run[0])
        self.assertIn('-p Nice=-15', run[0])
        self.assertIn('hold --khz 2160000', run[0])
        self.assertNotIn('--governor', run[0])
        # Already triggered this boot: trigger-cold.sh was not run again.
        self.assertIn('already triggered this boot', summary)

    def test_rerun_with_the_hold_running_does_not_restart_it(self):
        def held(s):
            s['hold_active'] = 'active'
            s['status']['held'] = True
        code, summary = self.run_script(held)
        self.assertEqual(code, 0, summary)
        self.assertFalse(any('systemd-run' in c or 'preflight' in c for c, _ in self.commands))
        self.assertIn('already held', self.line(summary, 'cpu'))

    def test_wrong_entry_stops_before_anything_else(self):
        def other(s):
            s['identity'] = s['identity'].replace(' apple_soc_cpufreq.a9_tuning=1', '')
        code, summary = self.run_script(other)
        self.assertEqual(code, 1)
        self.assertIn('apple_soc_cpufreq.a9_tuning=1 x0', self.line(summary, 'entry'))
        self.assertFalse(any('cold_release' in c or 'a9-oc-hold' in c for c, _ in self.commands))

    def test_full_turbo_table_fails_the_entry(self):
        def full(s):
            s['identity'] += 'opp opp-r6-2208000 2208000000 14 1\n'
        code, summary = self.run_script(full)
        self.assertEqual(code, 1)
        self.assertIn('not the showcase one', summary)

    def test_aop_ch3td_or_no_stream_fails(self):
        def ch3td(s):
            s['aop'] = s['aop'].replace('== iio', '[ 46.0] SPI0 CH3TD begin pa=0x210100000\n== iio')
        code, summary = self.run_script(ch3td)
        self.assertEqual(code, 1)
        self.assertIn('SPI0 CH3TD on a cold boot', self.line(summary, 'aop'))

        def frozen(s):
            s['aop'] = s['aop'].replace('reports=641', 'reports=611')
        code, summary = self.run_script(frozen)
        self.assertIn('reports not increasing', self.line(summary, 'aop'))

    def test_firmware_mismatch_is_installed(self):
        def missing(s):
            s['firmware'] = ''
        code, summary = self.run_script(missing)
        tees = [c for c, _ in self.commands if 'tee /lib/firmware/apple/aop/' in c]
        self.assertEqual(len(tees), 4)
        self.assertEqual(code, 1)  # the fake keeps reporting nothing installed
        self.assertIn('differs after install', self.line(summary, 'firmware'))

    def test_hot_preflight_fails_cpu_without_starting(self):
        def hot(s):
            s['preflight'] = dict(ok=False, checks=[dict(check='temperatures', ok=False,
                                                         detail='hottest 86000 milliC; cool below 85000 first')])
        code, summary = self.run_script(hot)
        self.assertEqual(code, 1)
        self.assertIn('cool below 85000', self.line(summary, 'cpu'))
        self.assertFalse(any('systemd-run' in c or 'load-test' in c for c, _ in self.commands))

    def test_load_not_reaching_the_target_fails(self):
        def slow(s):
            s['load'].update(ok=False, at_target=0, median_khz=1800000, error=None)
        code, summary = self.run_script(slow)
        self.assertEqual(code, 1)
        self.assertIn('0/32 samples', self.line(summary, 'cpu'))

    def test_rotation_lock_and_als_off_are_warnings(self):
        def soft(s):
            s['rotate'] = s['rotate'].replace('"locked": false', '"locked": true')
            s['als'] = s['als'].replace('enabled=on', 'enabled=switched-on')
        code, summary = self.run_script(soft)
        self.assertEqual(code, 0, summary)
        self.assertTrue(self.line(summary, 'rotate').startswith('WARN'))
        self.assertTrue(self.line(summary, 'als').startswith('WARN'))

    def test_no_accelerometer_fails_rotate(self):
        def none(s):
            s['rotate'] = 'proxy=active\naccelerometer=b false\nunit=active\nstatus=\n'
        code, summary = self.run_script(none)
        self.assertEqual(code, 1)
        self.assertIn('HasAccelerometer=b false', self.line(summary, 'rotate'))

    def test_users_own_fastfetch_config_is_kept(self):
        def own(s):
            s['fastfetch_remote'] = 'abc\n{ "my": "config" }\n'
        code, summary = self.run_script(own)
        self.assertFalse(any('config.jsonc.tmp' in c for c, _ in self.commands))
        self.assertTrue(self.line(summary, 'fastfetch').startswith('WARN'))

    def test_stop_cpu(self):
        code, summary = self.run_script(args=['--stop-cpu'])
        self.assertEqual(code, 0, summary)
        self.assertIn('hold stopped', summary)
        self.assertTrue(any('systemctl stop linux-a9-oc-hold' in c for c, _ in self.commands))
        self.assertFalse(any('cold_release' in c for c, _ in self.commands))


if __name__ == '__main__':
    raise SystemExit(not unittest.main(exit=False, verbosity=1).result.wasSuccessful())
