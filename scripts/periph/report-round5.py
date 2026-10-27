#!/usr/bin/env python3
"""Record Round 5 artifact/test provenance and fill the current report hashes."""
import hashlib
import json
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
    assert len((ROOT / 'patches/periph/series').read_text().splitlines()) == 52
    assert 'PASS: 52 patches replay' in (BUILD / 'round5-export.log').read_text()
    checks = (BUILD / 'round5-final-checks.log').read_text()
    assert not any(p in checks for p in ('WARNING:', 'ERROR:', 'CHECK:'))
    assert checks.count('total: 0 errors, 0 warnings, 0 checks') == 2
    assert 'PASS: source-aware charging linked' in checks
    sources = {}
    for line in (BUILD / 'round5-sources.sha256').read_text().splitlines():
        digest, name = line.split()
        sources[Path(name).name] = digest
    charger_sha = sha(ROOT / 'scripts/periph/kernel/apple_j71t_charger.c')
    battery_sha = sha(BUILD / 'round5-battery/bq27xxx_battery.c')
    assert sources['apple_j71t_charger.c'] == charger_sha
    assert sources['bq27xxx_battery.c'] == battery_sha
    assert sources['power_supply_sysfs.c'] == sha(BUILD / 'round5-battery/power_supply_sysfs.c')
    assert charger_sha in (BUILD / 'round5-charging-tests.log').read_text()
    assert battery_sha in (BUILD / 'round5-battery-tests.log').read_text()
    assert 'PASS: 13 firmware files' in (BUILD / 'round5-firmware-verification.log').read_text()
    assert 'one gzip stream / two newc archives' in (BUILD / 'round5-initramfs.log').read_text()
    archive = json.loads((BUILD / 'initramfs-manifest.json').read_text())
    assert archive['gzip_members'] == 1 and archive['newc_archives'] == 2
    names = ['build/kernel-periph/Image.gz',
             'build/kernel-periph/dtbs/s8003-j71t.dtb', 'build/initramfs-fw.gz']
    artifacts = {name: {'size': (ROOT / name).stat().st_size, 'sha256': sha(ROOT / name)} for name in names}
    assert archive['sha256'] == artifacts[names[2]]['sha256']
    report = {
        'round': 5, 'kernel_branch': 'a9/periph', 'kernel_head': head,
        'baseline': '6831bc701', 'previous_round_head': 'c7c3eb2d34fa75a29de764cc6c560dd6bc673d2f',
        'patch_count': 52, 'new_patches': [51, 52], 'apple_dtb_count': 104,
        'artifacts': artifacts, 'tested_source_sha256': sources,
        'initramfs': {'gzip_members': 1, 'newc_archives': 2,
                      'overlay_file_count': len(archive['firmware']) + len(archive['userspace']),
                      'peripherals_firmware_count': 13},
        'verified_offline': {
            'build': 'round5-build.log', 'patch_replay': 'round5-export.log',
            'strict_checkpatch_kernel_dt': 'round5-final-checks.log',
            'charger': 'round5-charging-tests.log: 56 original fault/guard cases, calibrated budgets, 400-mA fallback, explicit owner override, source/battery/transaction faults, rollback and PM revocation',
            'battery': 'round5-battery-tests.log: actual cached getter and uevent core; optional errors, valid zero, recovery and unchanged mandatory errors',
            'firmware': 'round5-firmware-verification.log', 'initramfs': 'round5-initramfs.log',
        },
        'host_re': 'docs/periph/round5-host-recon.md; no verified passive Mac-host allowance shortcut in the inspected paths',
        'pending_device': [
            'Nonzero 400-mA battery ceiling at nominal500-mA input; net current remains system-load limited',
            'Explicit 2400000 owner request over a detected500000 allowance, positive charging and revocation/restoration',
            'Optional charge_now omitted quietly when unavailable and valid numeric sample on recovery',
        ],
        'device_access_by_agent': False,
        'payload_command': 'TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz',
    }
    (BUILD / 'round5-final.json').write_text(json.dumps(report, indent=2) + '\n')
    substitutions = {'ROUND5_HEAD': head[:10], 'ROUND5_IMAGE': artifacts[names[0]]['sha256'],
                     'ROUND5_DTB': artifacts[names[1]]['sha256'],
                     'ROUND5_INITRAMFS': artifacts[names[2]]['sha256']}
    for name in ('README.md', 'round5.md', 'test-plan.md'):
        path = ROOT / 'docs/periph' / name
        text = path.read_text()
        for key, value in substitutions.items():
            text = text.replace(key, value)
        path.write_text(text)
    print(f'PASS: Round 5 {head}; 52 patches; 104 DTBs; provenance recorded')


if __name__ == '__main__':
    main()
