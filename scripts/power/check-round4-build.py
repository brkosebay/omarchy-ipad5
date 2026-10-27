#!/usr/bin/env python3
"""Verify exact integ b7e52e2624 + staged a9/power delta; no integration commit."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('r2_build', Path(__file__).with_name('check-round2-build.py'))
r2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r2)
directory = ROOT / 'build/kernel-power-r4-integ'
out = ROOT / 'build/power/oc-r4'
config = dict(re.findall(r'^(CONFIG_\w+)=(\S+)$', (directory / 'config').read_text(), re.M))
required = {name: 'y' for name in ('CONFIG_APPLE_S8003_THERMAL', 'CONFIG_APPLE_A9_TUNING',
    'CONFIG_CPU_THERMAL', 'CONFIG_CPU_FREQ_THERMAL', 'CONFIG_THERMAL_OF', 'CONFIG_HWMON',
    'CONFIG_WATCHDOG_SYSFS', 'CONFIG_APPLE_WATCHDOG', 'CONFIG_DEBUG_INFO_NONE', 'CONFIG_KEXEC_FILE')}
required['CONFIG_THERMAL_EMERGENCY_POWEROFF_DELAY_MS'] = '5000'
assert all(config.get(key) == val for key, val in required.items())
assert config.get('CONFIG_THERMAL_EMULATION', 'n') == 'n'
assert (directory / 'DIRTY').read_text().strip() == 'M  drivers/cpufreq/apple-a9-tuning.h'
image = gzip.decompress((directory / 'Image.gz').read_bytes())
assert image[56:60] == b'ARMd'
dtb = directory / 'dtbs/s8003-j71t.dtb'
nodes = r2.fdt(dtb.read_bytes())
phandle, = r2.words(nodes['/soc/thermal-sensor@20e0c0000']['phandle'])
cpu, = r2.words(nodes['/cpus/cpu@0']['phandle'])
for sensor, channel in (('soc0', 0), ('soc1', 1), ('tcc2', 5)):
    base = '/thermal-zones/apple-a9-' + sensor
    assert r2.words(nodes[base]['thermal-sensors']) == (phandle, channel)
    assert r2.words(nodes[base]['polling-delay']) == (100,)
    for kind, temp in (('passive', 80000), ('hot', 90000), ('critical', 100000)):
        assert r2.words(nodes[base + '/trips/' + kind]['temperature']) == (temp,)
    assert r2.words(nodes[base + '/cooling-maps/cpu']['cooling-device']) == (cpu, 0xffffffff, 0xffffffff)
for sensor in ('soc2', 'tcc0', 'tcc1'):
    assert '/thermal-zones/apple-a9-' + sensor not in nodes
for index, khz in enumerate((300000, 396000, 600000, 912000, 1200000, 1512000, 1800000,
                            1848000, 1896000, 1944000, 1992000, 2040000, 2088000, 2160000), 1):
    opp = nodes[f'/opp-table/opp{index:02d}']
    assert struct.unpack('>Q', opp['opp-hz'])[0] == khz * 1000
    assert r2.words(opp['opp-level']) == (index,)
    assert ('turbo-mode' in opp) == (index >= 8)
completion = re.search(r'done: (\d+) dtbs\s*$', (out / 'integrated-build.log').read_text())
assert completion and int(completion[1]) == len(list((directory / 'dtbs').glob('*.dtb')))
power = subprocess.check_output([str(ROOT / 'scripts/build-kernel.sh'), '--run', 'power',
    'git rev-parse HEAD'], text=True).strip()
paths = [directory / name for name in ('Image.gz', 'config', 'COMMIT', 'DIRTY')]
paths += [dtb, out / 'kernel.patch', out / 'validation.log', out / 'integrated-build.log']
print(json.dumps({'power_head': power, 'integration_base': (directory / 'COMMIT').read_text().strip(),
    'merged_tree': (out / 'merge-tree.txt').read_text().strip(),
    'integration_commit_created': False, 'staged_delta': 'drivers/cpufreq/apple-a9-tuning.h only',
    'patch_parent': 'b553f1ebaf', 'verified_config': required, 'trusted_sensor_mask': '0x23',
    'dtb_count': int(completion[1]), 'arm64_image_bytes': len(image),
    'device_access': 'none', 'artifacts': {str(p.relative_to(ROOT)):
        hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}, indent=2))
