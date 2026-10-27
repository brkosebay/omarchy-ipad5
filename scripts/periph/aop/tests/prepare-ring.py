#!/usr/bin/env python3
"""Host-only DT hint/bootargs tests; no SSH or payload generation."""
from pathlib import Path
import copy
import runpy
import shutil
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[4]
m = runpy.run_path(str(ROOT / 'scripts/periph/aop/prepare-ring.py'))
baseline = ('console=tty0 panic=0 apple_wdt.cold_restart=0 '
            'apple_pmgr.kexec_keep_on=1 nvme.write_window=1:100-200 '
            'apple_a9_aop.active=1 apple_a9_aop.ring_iio=1 linux_a9.entry=old')
line = m['cmdline'](baseline, 'test-scan', 'scan')
assert 'apple_a9_aop.active=0' in line and 'apple_a9_aop.active=1' not in line
assert 'apple_a9_aop.ring_iio=0' in line and 'apple_a9_aop.ring_scan=1' in line
assert 'nvme.write_window=1:100-200' in line and 'linux_a9.entry=old' not in line
assert 'apple_a9_aop.ring_scan_early=0' in line
assert 'apple_a9_aop.ring_scan_reserved=0' in line
early = m['cmdline'](baseline, 'test-early', 'scan', True)
assert 'apple_a9_aop.ring_scan_early=1' in early and 'apple_a9_aop.active=0' in early
reserved = m['cmdline'](baseline, 'test-reserved', 'scan', True, True)
assert 'apple_a9_aop.ring_scan_early=1' in reserved
assert 'apple_a9_aop.ring_scan_reserved=1' in reserved
assert 'apple_a9_aop.active=0' in reserved and 'apple_a9_aop.ring_iio=0' in reserved
target_line = m['cmdline'](baseline, 'test-target', 'target',
                           buffers=((0x840000040, 0x8000), (0x850000000, 0x4000)))
assert 'apple_a9_aop.ring_pa=0x840000040,0x850000000' in target_line
assert 'apple_a9_aop.ring_pa_size=0x8000,0x4000' in target_line
assert 'apple_a9_aop.active=0' in target_line and 'apple_a9_aop.ring_scan=0' in target_line
assert 'apple_a9_aop.ring_iio=0' in target_line and 'panic=0' in target_line
for buffers in ((), ((0, 1),), ((1, 0),), ((1, 65537),), (((1 << 64) - 1, 1),), ((1, 1),) * 5):
    try:
        m['cmdline'](baseline, 'bad-target', 'target', buffers=buffers)
    except ValueError:
        pass
    else:
        raise AssertionError(f'Bad target buffers accepted: {buffers}')
try:
    m['cmdline'](baseline, 'test-iio', 'iio', False, True)
except ValueError:
    pass
else:
    raise AssertionError('Reserved E1 option accepted for IIO')
try:
    m['cmdline'](baseline, 'test-iio', 'iio', True)
except ValueError:
    pass
else:
    raise AssertionError('Early IIO accepted')
for missing in ('panic=0', 'apple_wdt.cold_restart=0', 'apple_pmgr.kexec_keep_on=1'):
    try:
        m['cmdline'](baseline.replace(missing, ''), 'test', 'scan')
    except ValueError:
        pass
    else:
        raise AssertionError(f'Missing protection accepted: {missing}')
dtb = ROOT / 'build/kernel-aop/dtbs/s8003-j71t.dtb'
if not dtb.exists():
    raise SystemExit('Build aop DTBs first')
original = dtb.read_bytes()
with tempfile.TemporaryDirectory(prefix='aop-ring-dt-') as tmp:
    target = Path(tmp) / 'dtb'
    shutil.copyfile(dtb, target)
    # Non-page-aligned rings need BOTH covering 16 KiB pages reserved.
    hint = m['reserve'](target, 0x83fffffc0)
    node = '/reserved-memory/aop-inherited-ring@83fffc000'
    assert m['fdtget'](target, node, 'reg') == ['8', '3fffc000', '0', '8000']
    assert m['fdtget'](target, node, 'no-map') == []
    assert m['fdtget'](target, node, 'compatible', required=False) is None
    assert m['fdtget'](target, m['OBSERVER'], 'apple,ring-offset') == ['3fc0']
    assert '0x83fffffc0' in hint
    # Exercise the real kexec merger with runtime RAM and colliding phandles.
    f = runpy.run_path(str(ROOT / 'scripts/boot/device/linux-a9-fdt-merge'))
    old = f['load'](dtb)[0]
    incoming = f['load'](target)[0]
    running = copy.deepcopy(old)
    runtime = f['Node']('runtime-firmware')
    high = max(f['phandle_of'](n) or 0 for _, n in f['walk'](old))
    runtime.props['phandle'] = struct.pack('>I', high + 1)
    running.children['reserved-memory'].children[runtime.name] = runtime
    running.children['chosen'].props['memory-region'] = struct.pack('>I', high + 1)
    memory = next(n for n in running.children if n.startswith('memory@'))
    running.children[memory].props['reg'] = struct.pack('>IIII', 8, 0, 0, 0x80000000)
    errors, _, _ = f['merge'](old, running, incoming)
    assert not errors, errors
    ix = f['index'](incoming)
    assert ix[node].props['no-map'] == b''
    assert ix[m['OBSERVER']].props['memory-region'] == struct.pack(
        '>I', f['phandle_of'](ix[node]))
    assert ix['/chosen'].props['memory-region'] != ix[m['OBSERVER']].props['memory-region']
    # Reserved E2 -> reserved E2 warm boot retains the same ring and RAM fixup.
    pristine = f['load'](target)[0]
    warm = copy.deepcopy(pristine)
    errors, _, _ = f['merge'](pristine, incoming, warm)
    assert not errors, errors
    assert f['index'](warm)[node].props['reg'] == ix[node].props['reg']
    try:
        m['reserve'](target, 0x83fffffc0)
    except ValueError:
        pass
    else:
        raise AssertionError('Existing ring hint accepted')
    for pa in (0, 1, (1 << 48)):
        shutil.copyfile(dtb, target)
        try:
            m['reserve'](target, pa)
        except ValueError:
            pass
        else:
            raise AssertionError('Bad PA accepted')
assert original == dtb.read_bytes()
print('PASS: explicit DT hint, covering pages, no-map, baseline protections; real kexec merger retains reservation and RAM fixups across phandle collisions')
