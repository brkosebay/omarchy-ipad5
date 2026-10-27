#!/usr/bin/env python3
"""Offline charge7 payload/entry, preserving usb-host5b command line and initrd."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/charge7'
KERNEL = ROOT / 'build/kernel-charge'
BASE = ROOT / 'build/usb-host5b'
PARENT = '2801e838fc9a26e418b987aea173909db1d254ea'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, **kwargs):
    subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def main():
    prev = json.loads((BASE / 'manifest.json').read_text())
    assert prev['kernel_commit'] == PARENT
    for name, digest in prev['base_inputs'].items():
        assert sha(ROOT / name) == digest, name
    for name, digest in prev['files'].items():
        if name.startswith('build/usb-host5b/entry/'):
            assert sha(ROOT / name) == digest, name
    assert not (KERNEL / 'DIRTY').read_text().strip()
    commit = (KERNEL / 'COMMIT').read_text().split()[0]
    assert commit == (OUT / 'branch.txt').read_text().split()[0]
    assert (KERNEL / 'config').read_bytes() == (ROOT / 'build/kernel-usb-host5b/config').read_bytes()
    config = (KERNEL / 'config').read_text().splitlines()
    assert 'CONFIG_DEBUG_INFO_NONE=y' in config and 'CONFIG_DEBUG_INFO=y' not in config
    entry = 'charge7-' + commit[:10]
    before = (BASE / 'entry/cmdline').read_text().split()
    assert sum(t.startswith('linux_a9.entry=') for t in before) == 1
    tokens = [f'linux_a9.entry={entry}' if t.startswith('linux_a9.entry=') else t for t in before]
    assert tokens.count('pcie_apple_h9p.ans_keep_power=1') == 1
    assert sum(t.startswith('pcie_apple_h9p.ans_keep_power=') for t in tokens) == 1
    assert not any(t.startswith('systemd.debug_shell') for t in tokens)
    assert [t for t in tokens if not t.startswith('linux_a9.entry=')] == [
        t for t in before if not t.startswith('linux_a9.entry=')]
    assert tokens[:3] == ['earlycon', 'console=tty0', 'loglevel=7']
    cmdline = ' '.join(tokens) + '\n'
    (OUT / 'cmdline').write_text(cmdline)
    payload = OUT / f'm1n1-linux-{entry}.bin'
    env = dict(os.environ, TREE='charge', M1N1=str(ROOT / 'build/m1n1-boot/m1n1.bin'),
               M1N1_VARS='psci=1', PAYLOAD_OUT=str(payload))
    run('bash', 'scripts/mkpayload.sh', str(BASE / 'entry/initramfs.gz'), *tokens[3:], env=env)
    dest = OUT / 'entry'
    run('python3', '-B', '-I', 'scripts/boot/prepare-entry.py', str(payload), str(dest),
        '--board', 'apple,j71t')
    (dest / 'COMMIT').write_bytes((KERNEL / 'COMMIT').read_bytes())
    assert (dest / 'cmdline').read_text() == cmdline
    image = gzip.decompress((KERNEL / 'Image.gz').read_bytes())
    assert gzip.decompress((dest / 'Image.gz').read_bytes()) == image
    assert gzip.decompress((dest / 'initramfs.gz').read_bytes()) == gzip.decompress(
        (BASE / 'entry/initramfs.gz').read_bytes())
    assert (dest / 'dtb').read_bytes() == (KERNEL / 'dtbs/s8003-j71t.dtb').read_bytes()
    for marker in (b'charge7_state', b'pmic-input-reduced', b'vbus_mv=unavailable',
                   b'apple_s8000_usb_role_monitor', b'lightning_state', b'camera adapter',
                   b'usb_ctl_force', b'ans_keep_power', b'MSI resumed after vector drain'):
        assert marker in image, marker
    files = sorted(p for p in dest.iterdir() if p.is_file() and p.name != 'SHA256SUMS')
    (dest / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  ./{p.name}\n' for p in files))
    artifacts = [payload, KERNEL / 'config', KERNEL / 'RELEASE', *files,
                 *(KERNEL / 'dtbs').glob('s800*-j7[12]*.dtb')]
    manifest = dict(entry=entry, kernel_commit=commit, parent_commit=PARENT,
                    parent_entry=prev['entry'], base_inputs=prev['base_inputs'],
                    hardware_tested=False, voltage_adaptive_backoff=False,
                    pmu_input_reduction_backoff=True, config_change='none (usb-host5b config)',
                    payload=str(payload.relative_to(ROOT)), payload_sha256=sha(payload),
                    entry_dir=str(dest.relative_to(ROOT)),
                    files={str(p.relative_to(ROOT)): sha(p) for p in artifacts})
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (OUT / 'entry-name').write_text(entry + '\n')
    (OUT / 'payload-path').write_text(str(payload.relative_to(ROOT)) + '\n')
    # mkpayload's deterministic temporary copy is not another delivered payload.
    (ROOT / f'build/.initrd-single-{payload.stem}.gz').unlink(missing_ok=True)
    print(f'PASS {entry}: payload sha256 {sha(payload)}; unchanged cmdline/initrd; new DTBs')


if __name__ == '__main__':
    main()
