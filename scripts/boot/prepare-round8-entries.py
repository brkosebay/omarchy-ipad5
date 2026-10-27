#!/usr/bin/env python3
"""Package the identical Round 8 build into three mode-selectable warm entries."""
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
    out = ROOT / 'build/boot/round8'
    kernel = ROOT / 'build/kernel-bootr8-check'
    old = ROOT / 'build/kernel-bootr7-check'
    head = (out / 'boot-head.txt').read_text().strip()
    base = (out / 'integration-base.txt').read_text().strip()
    assert (kernel / 'COMMIT').read_text().startswith(base)
    assert (old / 'config').read_bytes() == (kernel / 'config').read_bytes()
    assert (old / 'dtbs/s8003-j71t.dtb').read_bytes() == (kernel / 'dtbs/s8003-j71t.dtb').read_bytes()
    assert (out / 'kernel.patch').read_bytes() == (out / 'integration.patch').read_bytes()
    config = (kernel / 'config').read_text()
    for symbol in ('CONFIG_NVME_PCI_READ_ONLY=y', 'CONFIG_NVME_PCI_WRITE_WINDOW=y',
                   'CONFIG_DEBUG_INFO_NONE=y', 'CONFIG_PSTORE_RAM=y', 'CONFIG_PSTORE_CONSOLE=y'):
        assert symbol in config, symbol
    image = gzip.decompress((kernel / 'Image.gz').read_bytes())
    for marker in (b'DART checked reinit:', b'health gate partition/data NSID 1',
                   b'handoff already-enabled L0', b'one recovery using %s bring-up'):
        assert marker in image, marker
    passed, skipped = 0, 0
    for name in ('handoff', 'ans', 'ans-io', 'recovery', 'peripherals', 'policy', 'update', 'fdt'):
        log = (out / f'{name}.log').read_text()
        assert '\nOK' in log and 'FAILED (' not in log, name
        count = int(re.search(r'^Ran (\d+) tests? in ', log, re.M)[1])
        skip = re.search(r'^OK \(skipped=(\d+)\)', log, re.M)
        skip = int(skip[1]) if skip else 0
        passed += count - skip
        skipped += skip
    assert 'done:' in (out / 'build-final.log').read_text()
    assert 'done:' in (out / 'integration-build.log').read_text()
    saved = out / 'initramfs.gz'
    shutil.copyfile(ROOT / 'build/boot/initramfs-omarchy.gz', saved)
    entries = {}
    for mode in ('handoff', 'full-cycle', 'round7'):
        name = f'round8-{mode}-{head[:10]}'
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
        (entry / 'COMMIT').write_text(f'{head} a9/boot on detached integration {base}\n')
        (entry / 'SOURCE').write_text(f'Exact overlay {sha(out / "kernel.patch")}\n')
        files = sorted(f for f in entry.iterdir() if f.is_file() and f.name != 'SHA256SUMS')
        (entry / 'SHA256SUMS').write_text(''.join(f'{sha(f)}  ./{f.name}\n' for f in files))
        entries[mode] = {'name': name, 'directory': str(entry.relative_to(ROOT)), 'cmdline': ' '.join(command)}
    paths = [kernel / 'Image.gz', kernel / 'dtbs/s8003-j71t.dtb', kernel / 'config',
             out / 'kernel.patch', out / 'integration.patch', saved]
    for e in entries.values():
        paths.extend(sorted(f for f in (ROOT / e['directory']).iterdir() if f.is_file()))
    files = {str(f.relative_to(ROOT)): {'sha256': sha(f), 'bytes': f.stat().st_size} for f in paths}
    manifest = {'kernel_head': head, 'integration_base': base,
                'integration_source': 'detached exact Round 7+8 overlay',
                'default_mode': 'handoff', 'entries': entries, 'files': files,
                'validation': {'offline_tests_passed': passed, 'optional_tests_skipped': skipped,
                               'new_production_tests': 10, 'hardware_tested': False,
                               'standalone_dtbs': 104, 'integration_dtbs': 105,
                               'tested_config_and_j71t_dtb_unchanged': True},
                'device_access': False, 'workspace_commits': False, 'pushed': False}
    (out / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (out / 'SHA256SUMS').write_text(''.join(f'{v["sha256"]}  {k}\n' for k, v in files.items()))
    for mode, entry in entries.items():
        print(mode, entry['name'], entry['directory'])


if __name__ == '__main__':
    main()
