#!/usr/bin/env python3
"""Record local Round 4 build provenance and fill the current report hashes."""
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
    assert len(head) == 40
    assert not (KERNEL / 'DIRTY').read_text().strip()
    assert len(list((KERNEL / 'dtbs').glob('*.dtb'))) == 104
    series = (ROOT / 'patches/periph/series').read_text().splitlines()
    assert len(series) == 50
    export = (BUILD / 'round4-export.log').read_text()
    assert 'PASS: 50 patches replay' in export
    checkpatch = (BUILD / 'round4-patches-checkpatch.log').read_text()
    assert not any(p in checkpatch for p in ('WARNING:', 'ERROR:', 'CHECK:'))
    assert checkpatch.count('total: 0 errors, 0 warnings, 0 checks') == 8
    assert (BUILD / 'round4-kernel-verification.log').read_text().startswith('PASS:')
    archive = json.loads((BUILD / 'initramfs-manifest.json').read_text())
    assert archive['gzip_members'] == 1 and archive['newc_archives'] == 2
    paths = ['build/kernel-periph/Image.gz',
             'build/kernel-periph/dtbs/s8003-j71t.dtb', 'build/initramfs-fw.gz']
    artifacts = {p: {'size': (ROOT / p).stat().st_size, 'sha256': sha(ROOT / p)} for p in paths}
    assert artifacts[paths[2]]['sha256'] == archive['sha256']
    sources = {name: sha(ROOT / 'scripts/periph/kernel' / name)
               for name in ('apple_j71t_charger.c', 'apple_tristar_power.c')}
    assert sources['apple_j71t_charger.c'] in (BUILD / 'round4-charging-tests/latest.log').read_text()
    assert sources['apple_tristar_power.c'] in (BUILD / 'round4-source-tests.log').read_text()
    manifest = {
        'round': 4, 'kernel_branch': 'a9/periph', 'kernel_head': head,
        'baseline': '6831bc701', 'patch_count': 50, 'apple_dtb_count': 104,
        'artifacts': artifacts, 'tested_source_sha256': sources,
        'initramfs': {'gzip_members': 1, 'newc_archives': 2,
                      'overlay_file_count': len(archive['firmware']) + len(archive['userspace']),
                      'peripherals_firmware_count': 13,
                      'unchanged_touch_files_outside_claim': 2},
        'verification': {
            'kernel_build': 'round4-build.log', 'kernel_and_dt': 'round4-kernel-verification.log',
            'bindings_and_dt_schema': 'round4-bindings.log (optional yamllint unavailable)',
            'patch_replay': 'round4-export.log', 'strict_checkpatch': 'round4-patches-checkpatch.log',
            'bluetooth': '114 ASan/UBSan assertions; round4-bt-usb-final-tests.log',
            'usb': '160 ASan/UBSan assertions and fresh idempotent replay; round4-usb/validation.log',
            'charging': '56 original fault/guard cases plus calibrated/source/automatic/fault-latch cases; round4-charging-tests/latest.log',
            'adc_mux': '4096 raw ADC fixtures, classifier boundaries and 13 fault paths; round4-source-tests.log',
            'firmware': '15 tests; round4-firmware-tests.log; 13 source-matched files; round4-firmware-verification.log',
            'initramfs': '2 tests; round4-initramfs-tests.log; round4-initramfs.log',
        },
        'payload_command': 'TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz',
        'device_access_by_agent': False,
        'pending_hardware': [
            'Mac Apple current grant delivery to existing 05ac:4142 gadget',
            'Automatic high-current behavior and conservative suspend/resume policy',
            'ADC ownership/completion with inherited mask and bounded mux timing',
            'Boot-time real Bluetooth identity and normal-log TX-power warning removal',
            'Absence of PMIC bad-cell warnings and UPower charge_now plausibility',
        ],
    }
    (BUILD / 'round4-final.json').write_text(json.dumps(manifest, indent=2) + '\n')
    replacements = {'ROUND4_HEAD': head[:10], 'ROUND4_IMAGE': artifacts[paths[0]]['sha256'],
                    'ROUND4_DTB': artifacts[paths[1]]['sha256'],
                    'ROUND4_INITRAMFS': artifacts[paths[2]]['sha256']}
    for name in ('round4.md', 'README.md', 'test-plan.md'):
        path = ROOT / 'docs/periph' / name
        text = path.read_text()
        for key, value in replacements.items():
            text = text.replace(key, value)
        path.write_text(text)
    print(f'PASS: Round 4 {head}, 50 patches, 104 DTBs; provenance recorded')


if __name__ == '__main__':
    main()
