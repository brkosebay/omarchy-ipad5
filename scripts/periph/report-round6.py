#!/usr/bin/env python3
"""Record Round 6 artifact/test provenance and fill the current report hashes."""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build/periph'
KERNEL = ROOT / 'build/kernel-periph'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    head = (KERNEL / 'COMMIT').read_text().split()[0]
    assert len(head) == 40 and not (KERNEL / 'DIRTY').read_text().strip()
    assert len(list((KERNEL / 'dtbs').glob('*.dtb'))) == 104
    assert len((ROOT / 'patches/periph/series').read_text().splitlines()) == 55
    assert 'PASS: 55 patches replay' in (BUILD / 'round6-export.log').read_text()
    checks = (BUILD / 'round6-final-checks.log').read_text()
    assert not any(p in checks for p in ('WARNING:', 'ERROR:', 'CHECK:'))
    assert checks.count('total: 0 errors, 0 warnings, 0 checks') == 3
    assert 'PASS: source-aware charging linked' in checks
    sources = {}
    for line in (BUILD / 'round6-sources.sha256').read_text().splitlines():
        digest, name = line.split()
        sources[Path(name).name] = digest
    charger_sha = sha(ROOT / 'scripts/periph/kernel/apple_j71t_charger.c')
    battery_sha = sha(BUILD / 'round6-battery/bq27xxx_battery.c')
    assert sources['apple_j71t_charger.c'] == charger_sha
    assert sources['bq27xxx_battery.c'] == battery_sha
    assert sources['power_supply_sysfs.c'] == sha(BUILD / 'round6-battery/power_supply_sysfs.c')
    assert charger_sha in (BUILD / 'round6-charging-tests.log').read_text()
    assert battery_sha in (BUILD / 'round6-battery-tests.log').read_text()
    for name in ('bq27xxx_battery.h', 'power_supply_core.c', 'thermal_core.c'):
        assert sources[name] == sha(BUILD / 'round6-battery' / name)
        assert sources[name] in (BUILD / 'round6-battery-tests.log').read_text()
    assert 'one gzip stream / two newc archives' in (BUILD / 'round6-initramfs.log').read_text()
    archive = json.loads((BUILD / 'initramfs-manifest.json').read_text())
    assert archive['gzip_members'] == 1 and archive['newc_archives'] == 2
    names = ['build/kernel-periph/Image.gz',
             'build/kernel-periph/dtbs/s8003-j71t.dtb', 'build/initramfs-fw.gz']
    artifacts = {name: {'size': (ROOT / name).stat().st_size, 'sha256': sha(ROOT / name)} for name in names}
    assert archive['sha256'] == artifacts[names[2]]['sha256']
    report = {
        'round': 6, 'kernel_branch': 'a9/periph', 'kernel_head': head,
        'baseline': '6831bc701', 'previous_round_head': '41c03c2e909081503d89694954a9950c2e13d80e',
        'patch_count': 55, 'new_patches': [53, 54, 55], 'apple_dtb_count': 104,
        'artifacts': artifacts, 'tested_source_sha256': sources,
        'initramfs': {'gzip_members': 1, 'newc_archives': 2,
                      'overlay_file_count': len(archive['firmware']) + len(archive['userspace']),
                      'peripherals_firmware_count': 13},
        'verified_offline': {
            'build': 'round6-build.log', 'patch_replay': 'round6-export.log',
            'strict_checkpatch_kernel_dt': 'round6-final-checks.log',
            'charger': 'round6-charging-tests.log: original 56 fault/guard cases and prior owner tests; actual battery guard, 65 no-data startup ticks, unassisted recovery, bounded decision logs and retained hardware latches',
            'battery': 'round6-battery-tests.log: actual refresh/getter/uevent/thermal functions; five mandatory startup failures, 1-s retry,100 quiet rechecks per fault, valid recovery and preserved runtime errors',
            'firmware': 'unchanged source selection from Round 5; archive byte-match verified', 'initramfs': 'round6-initramfs.log',
        },
        'verified_previous_device': 'integ10: periph41c03c2e90; owner2400-mA request gives +1.16 A; numeric charge_now and uevents valid',
        'pending_device': [
            'Automatic calibrated475000 input /400000 battery ceiling after first complete gauge sample, without owner request',
            'Initial cache failure reported as no data; quiet thermal retry; decision logs on wait/guard/fault',
            'Retained owner override and runtime-fault behavior',
        ],
        'device_access_by_agent': False,
        'payload_command': 'TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz',
    }
    (BUILD / 'round6-final.json').write_text(json.dumps(report, indent=2) + '\n')
    substitutions = {'ROUND6_HEAD': head[:10], 'ROUND6_IMAGE': artifacts[names[0]]['sha256'],
                     'ROUND6_DTB': artifacts[names[1]]['sha256'],
                     'ROUND6_INITRAMFS': artifacts[names[2]]['sha256']}
    for name in ('README.md', 'round6.md', 'test-plan.md'):
        path = ROOT / 'docs/periph' / name
        text = path.read_text()
        for key, value in substitutions.items():
            text = text.replace(key, value)
        if name == 'test-plan.md':
            text = re.sub(r'Expected kernel head: `[^`]+`', f'Expected kernel head: `{head[:10]}`', text)
            for artifact, details in artifacts.items():
                text = re.sub(r'(?m)^[a-f0-9]{64}(  ' + re.escape(artifact) + r')$',
                              lambda match: details['sha256'] + match.group(1), text)
        path.write_text(text)
    print(f'PASS: Round 6 {head}; 55 patches; 104 DTBs; provenance recorded')


if __name__ == '__main__':
    main()
