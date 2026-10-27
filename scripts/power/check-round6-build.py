#!/usr/bin/env python3
"""Verify the detached integration build and unchanged stock DT trip defaults."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('r2_build', Path(__file__).with_name('check-round2-build.py'))
r2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r2)
directory = ROOT / 'build/kernel-power-r6-integ'
out = ROOT / 'build/power/oc-r6'
config = dict(re.findall(r'^(CONFIG_\w+)=(\S+)$', (directory / 'config').read_text(), re.M))
required = {name: 'y' for name in ('CONFIG_APPLE_S8003_THERMAL', 'CONFIG_APPLE_A9_TUNING',
    'CONFIG_CPU_THERMAL', 'CONFIG_CPU_FREQ_THERMAL', 'CONFIG_THERMAL_OF', 'CONFIG_HWMON',
    'CONFIG_WATCHDOG_SYSFS', 'CONFIG_APPLE_WATCHDOG', 'CONFIG_DEBUG_INFO_NONE', 'CONFIG_KEXEC_FILE')}
required['CONFIG_THERMAL_EMERGENCY_POWEROFF_DELAY_MS'] = '5000'
assert all(config.get(key) == val for key, val in required.items())
assert config.get('CONFIG_THERMAL_EMULATION', 'n') == 'n'
changed = {'drivers/cpufreq/apple-a9-tuning.h', 'drivers/thermal/apple-s8003-thermal.c',
           'include/linux/soc/apple/s8003-power.h', 'arch/arm64/boot/dts/apple/s8003.dtsi',
           'drivers/cpufreq/apple-soc-cpufreq.c'}
assert set((directory / 'DIRTY').read_text().splitlines()) == {'M  ' + p for p in changed}
image = gzip.decompress((directory / 'Image.gz').read_bytes())
assert image[56:60] == b'ARMd'
assert b'thermal_profile_format=1 profile=' in image
assert b'profile_ready=' in image
assert b'clock_profile=s8003-r6-shared-slot' in image
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
assert r2.words(nodes['/opp-table/opp08']['opp-level']) == (15,)
for index in range(9, 15):
    assert r2.words(nodes[f'/opp-table/opp{index:02d}']['opp-level']) == (index,)
# Every advertised frequency must map to the same slot as the guarded helper.
import sys
sys.path.insert(0, str(ROOT / 'scripts/power'))
import ladder
opp_nodes = {int.from_bytes(props['opp-hz'], 'big') // 1000: props
             for name, props in nodes.items() if name.startswith('/opp-table/') and 'opp-hz' in props}
assert set(opp_nodes) == {300000, 396000, 600000, 912000, 1200000, 1512000, 1800000, *ladder.STEPS}
for index, khz in enumerate(ladder.STEPS):
    props = opp_nodes[khz]
    expected = 15 if index == 0 else min(index + 8, 14)
    assert r2.words(props['opp-level']) == (expected,)
    assert 'turbo-mode' in props
assert ladder.STEPS[-1] == 511 * 24000
completion = re.search(r'done: (\d+) dtbs\s*$', (out / 'integrated-build.log').read_text())
assert completion and int(completion[1]) == len(list((directory / 'dtbs').glob('*.dtb')))
tests = re.search(r'Ran (\d+) tests.*?\bOK\b', (out / 'validation.log').read_text(), re.S)
assert tests and int(tests[1]) >= 52
power = subprocess.check_output([str(ROOT / 'scripts/build-kernel.sh'), '--run', 'power',
    'test -z "$(git status --porcelain)"; git rev-parse HEAD'], text=True).strip()
assert power == (out / 'power-head.txt').read_text().strip()
paths = [directory / name for name in ('Image.gz', 'config', 'COMMIT', 'DIRTY')]
paths += [dtb, out / 'kernel.patch', out / 'validation.log', out / 'integrated-build.log',
          out / 'integration-input.config', out / 'checkpatch.log']
paths += [ROOT / 'scripts/power' / p for p in
          ('a9_thermal.py', 'ladder.py', 'stock-log.py', 'validate-round6.py',
           'install-round6.py', 'build-round6.sh', 'check-round6-build.py', 'seed-round6-cache.py',
           'kernel/apple-a9-tuning.h', 'kernel/apple-s8003-thermal.c', 'kernel/s8003-power.h',
           'validate-round2.py', 'validate-round3.py', 'validate-round4.py', 'validate-round5.py')]
print(json.dumps({'power_head': power,
    'integration_base': (directory / 'COMMIT').read_text().split()[0],
    'merged_tree': (out / 'merge-tree.txt').read_text().strip(),
    'integration_commit_created': False, 'staged_delta': sorted(changed),
    'patch_parent': '49c0339866', 'round6_parent': '20ddcb7f1c', 'verified_config': required,
    'trusted_sensor_mask': '0x23', 'dtb_count': int(completion[1]),
    'arm64_image_bytes': len(image), 'python_tests': int(tests[1]),
    'clock_profile': 's8003-r6-shared-slot', 'native_boost_slot_8': 'unchanged',
    'all_load_1848_slot': 15, 'extended_slot': 14,
    'pll_multiplier_bits': [12, 4], 'pll_multiplier_max': 511,
    'encoding_max_khz': ladder.PLL_MAX_KHZ, 'tested_hardware_max_khz': None,
    'quick_build': 'single incremental Image.gz + dtbs, seeded from round-5 ignored products',
    'sanitized_kernel_helpers': ['guard/voltage/MMIO', 'thermal trip profile application'],
    'stock_trips_millic': [80000, 90000, 100000],
    'sustained_trips_millic': [90000, 95000, 100000],
    'device_access': 'none', 'artifacts': {str(p.relative_to(ROOT)):
        hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}, indent=2))
