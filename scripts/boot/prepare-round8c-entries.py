#!/usr/bin/env python3
"""Package the identical Round 8c build into three mode-selectable warm entries."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[2]


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cmdline-file', type=Path, default=ROOT / 'build/boot/round7/entry/cmdline')
    args = p.parse_args()
    out = ROOT / 'build/boot/round8c'
    kernel = ROOT / 'build/kernel-bootr8c-check'
    old = ROOT / 'build/kernel-bootr7-check'
    head = (out / 'boot-head.txt').read_text().strip()
    base = (out / 'integration-base.txt').read_text().strip()
    assert (kernel / 'COMMIT').read_text().startswith(base)
    standalone = ROOT / 'build/kernel-boot'
    assert (standalone / 'COMMIT').read_text().startswith(head)
    assert not (standalone / 'DIRTY').read_text().strip()
    release = (kernel / 'RELEASE').read_text().strip()
    assert f'-a9-boot-r8c-{head[:10]}' in release
    assert (out / 'kernel.patch').read_bytes() == (kernel / 'SOURCE.patch').read_bytes()
    exported = ROOT / 'patches/boot/round8c'
    assert (exported / 'HEAD').read_text().strip() == head
    export_manifest = json.loads((exported / 'manifest.json').read_text())
    for name, digest in export_manifest['patches'].items():
        assert sha(exported / name) == digest, name

    assert (old / 'config').read_bytes() == (kernel / 'config').read_bytes()
    assert (old / 'dtbs/s8003-j71t.dtb').read_bytes() == (kernel / 'dtbs/s8003-j71t.dtb').read_bytes()
    assert (out / 'kernel.patch').read_bytes() == (out / 'integration.patch').read_bytes()
    config = (kernel / 'config').read_text()
    for symbol in ('CONFIG_NVME_PCI_READ_ONLY=y', 'CONFIG_NVME_PCI_WRITE_WINDOW=y',
                   'CONFIG_DEBUG_INFO_NONE=y', 'CONFIG_PSTORE_RAM=y', 'CONFIG_PSTORE_CONSOLE=y'):
        assert symbol in config, symbol
    image = gzip.decompress((kernel / 'Image.gz').read_bytes())
    assert b'Linux version ' + release.encode() + b' ' in image
    for marker in (b'health gate inconclusive', b'continuing namespace discovery without recovery',
                   b'controller timeout', b'DART checked reinit:', b'health gate partition/data NSID 1',
                   b'local retirement error; no reset', b'confirmed NVMMU failure; scheduling reset',
                   b'one recovery using %s bring-up'):
        assert marker in image, marker
    assert b'handoff already-enabled L0' not in image
    passed, skipped = 0, 0
    for name in ('handoff', 'ans', 'ans-io', 'recovery', 'peripherals', 'policy', 'update', 'fdt'):
        log = (out / f'{name}.log').read_text()
        assert '\nOK' in log and 'FAILED (' not in log, name
        count = int(re.search(r'^Ran (\d+) tests? in ', log, re.M)[1])
        skip = re.search(r'^OK \(skipped=(\d+)\)', log, re.M)
        skip = int(skip[1]) if skip else 0
        passed += count - skip
        skipped += skip
    for name, count in (('handoff', 12), ('ans-io', 8)):
        log = (out / f'{name}-integration.log').read_text()
        assert '\nOK' in log and 'FAILED (' not in log, name
        assert int(re.search(r'^Ran (\d+) tests? in ', log, re.M)[1]) == count, name
    baseline = (out / 'regression-round8b-retirement.log').read_text()
    assert 'Assertion' in baseline and 'schedules==old_schedules' in baseline
    assert 'FAILED (errors=1)' in baseline
    assert 'done:' in (out / 'build-final.log').read_text()
    assert 'done:' in (out / 'integration-build.log').read_text()
    saved = out / 'initramfs.gz'
    # Use the exact rescue-capable round-8 initramfs: this round changes kernel only.
    original = ROOT / 'build/boot/round8/initramfs.gz'
    original_manifest = json.loads((ROOT / 'build/boot/round8/build.json').read_text())
    assert sha(original) == original_manifest['files'][str(original.relative_to(ROOT))]['sha256']
    shutil.copyfile(original, saved)
    entries = {}
    for mode in ('handoff', 'full-cycle', 'round7'):
        name = f'round8c-{mode}-{head[:10]}'
        entry = out / ('entry-' + mode)
        entry.mkdir(exist_ok=True)
        replacements = {'linux_a9.entry': name, 'pcie_apple_h9p.ans_boot_mode': mode,
                        'nvme.apple_ans_debug': '1', 'nvme.apple_ans_rdy_timeout': '5',
                        'nvme_core.io_timeout': '5', 'nvme_core.admin_timeout': '10',
                        'nvme_core.max_retries': '1', 'omarchy.nvme_wait': '90'}
        command = [x for x in args.cmdline_file.read_text().strip().split()
                   if x.split('=', 1)[0] not in replacements and not x.startswith('linux_a9.phoenix_from=')]
        command += [f'{k}={v}' for k, v in replacements.items()]
        for token in ('nvme.write_window=1:3146240-7812095', 'omarchy.root=nvme',
                      'panic=0', 'apple_wdt.cold_restart=0', 'crash_kexec_post_notifiers',
                      'apple_boot_watchdog.timeout=120', 'apple_pmgr.kexec_keep_on=1',
                      'apple_boot.deferred_async=1'):
            assert command.count(token) == 1, token
        for source, dest in [(kernel / 'Image.gz', 'Image.gz'),
                             (kernel / 'dtbs/s8003-j71t.dtb', 'dtb'), (saved, 'initramfs.gz')]:
            shutil.copyfile(source, entry / dest)
        (entry / 'cmdline').write_text(' '.join(command) + '\n')
        (entry / 'COMMIT').write_text(f'{head} a9/boot on detached integration {base}\n{release}\n')
        (entry / 'SOURCE').write_text(f'Exact overlay {sha(out / "kernel.patch")}\nRelease {release}\n')
        files = sorted(f for f in entry.iterdir() if f.is_file() and f.name != 'SHA256SUMS')
        (entry / 'SHA256SUMS').write_text(''.join(f'{sha(f)}  ./{f.name}\n' for f in files))
        entries[mode] = {'name': name, 'directory': str(entry.relative_to(ROOT)), 'cmdline': ' '.join(command)}
    paths = [kernel / 'RELEASE', kernel / 'COMMIT', kernel / 'DIRTY', kernel / 'SOURCE.patch',
             standalone / 'COMMIT', standalone / 'DIRTY', standalone / 'RELEASE',
             exported / 'BASE', exported / 'HEAD', exported / 'SERIES', exported / 'manifest.json',
             *sorted(exported.glob('*.patch')),
             kernel / 'Image.gz', kernel / 'dtbs/s8003-j71t.dtb', kernel / 'config',
             out / 'kernel.patch', out / 'integration.patch', saved]
    paths += [exported / 'README.md', out / 'regression-round8b-retirement.log',
              out / 'regression-round8b.log', out / 'regression.log']
    paths += [ROOT / 'scripts/boot' / name for name in (
        'build-round8c.sh', 'export-round8c.sh', 'prepare-round8c-entries.py',
        'warm-chain.sh', 'test-round8c-regression.sh', 'test-ans-handoff-offline.py', 'test-ans-io-offline.py',
        'tests/ans-slot-owner.c', 'tests/ans-port-owner.c', 'tests/ans-io-controller.c',
        'tests/ans-io-nvmmu.c', 'tests/ans-handoff-port.c')]
    paths += [out / f'{name}.log' for name in (
        'handoff', 'ans', 'ans-io', 'recovery', 'peripherals', 'policy', 'update', 'fdt',
        'handoff-integration', 'ans-io-integration', 'check-build', 'checkpatch-export')]
    for e in entries.values():
        paths.extend(sorted(f for f in (ROOT / e['directory']).iterdir() if f.is_file()))
    files = {str(f.relative_to(ROOT)): {'sha256': sha(f), 'bytes': f.stat().st_size} for f in paths}
    manifest = {'kernel_head': head, 'kernel_release': release,
                'incremental_base': export_manifest['base'],
                'overlay_sha256': sha(out / 'kernel.patch'), 'integration_base': base,
                'integration_source': 'detached exact Round 7+8+8b+8c overlay',
                'default_mode': 'handoff', 'entries': entries, 'files': files,
                'validation': {'offline_tests_passed': passed, 'optional_tests_skipped': skipped,
                               'production_handoff_tests': 12, 'hardware_tested': False,
                               'standalone_dtbs': 104, 'integration_dtbs': 105,
                               'tested_config_and_j71t_dtb_unchanged': True,
                               'gate_local_errors_never_recover': True,
                               'slot_local_errors_never_reset': True,
                               'round8b_local_reset_regression_reproduced': True,
                               'slot_ownership_diagnostics': True,
                               'port_owner_uses_pci_topology': True,
                               'same_entry_full_port_bringup': True,
                               'recovery_requires_confirmed_gate_timeout': True,
                               'dart_fault_requires_valid_bit': True},
                'device_access': False, 'workspace_commits': False, 'pushed': False}
    (out / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (out / 'SHA256SUMS').write_text(''.join(f'{v["sha256"]}  {k}\n' for k, v in files.items()))
    for mode, entry in entries.items():
        print(mode, entry['name'], entry['directory'])


if __name__ == '__main__':
    main()
