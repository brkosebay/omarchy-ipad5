#!/usr/bin/env python3
"""Package irqfix-usb4: usb-host4 plus the NVMe shared-MSI fix, nothing else."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
K = ROOT / 'build/kernel-kexec-irqfix-usb4'
PREV = ROOT / 'build/kernel-usb-host4'
PREV_OUT = ROOT / 'build/usb-host4'
OUT = ROOT / 'build/kexec-irqfix'
PARENT = '09e836312a'
RESCUE = 'nvme.apple_ans_irq_rescue_ms=200'


def run(*args, **kwargs):
    subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    prev = json.loads((PREV_OUT / 'manifest.json').read_text())
    for name, digest in prev['base_inputs'].items():
        assert sha256(ROOT / name) == digest, name
    assert prev['kernel_commit'].startswith(PARENT)
    assert not (K / 'DIRTY').read_text().strip(), 'dirty kernel source'
    commit = (K / 'COMMIT').read_text().split()[0]
    assert commit == (OUT / 'branch.txt').read_text().split()[0]
    assert (K / 'config').read_bytes() == (PREV / 'config').read_bytes()
    for dtb in sorted((PREV / 'dtbs').glob('*.dtb')):
        assert (K / 'dtbs' / dtb.name).read_bytes() == dtb.read_bytes(), dtb.name

    entry = f'irqfix-usb4-{commit[:10]}'
    tokens = (PREV_OUT / 'cmdline').read_text().split()
    assert sum(t.startswith('linux_a9.entry=') for t in tokens) == 1
    tokens = [f'linux_a9.entry={entry}' if t.startswith('linux_a9.entry=') else t
              for t in tokens]
    assert not any(t.startswith('nvme.apple_ans_irq_rescue_ms=') for t in tokens)
    for required in ('pcie_apple_h9p.ans_keep_power=1', 'omarchy.root=nvme',
                     'nvme_core.io_timeout=5', 'nvme_core.admin_timeout=10'):
        assert tokens.count(required) == 1, required
    cmdline = ' '.join(tokens)
    assert len(cmdline.encode()) < 1000
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'cmdline').write_text(cmdline + '\n')
    # Same payload, rescue enabled: pass as kexec-host.sh's cmdline argument.
    rescue_entry = f'{entry}-rescue'
    rescue = [f'linux_a9.entry={rescue_entry}' if t.startswith('linux_a9.entry=') else t
              for t in tokens] + [RESCUE]
    assert len(' '.join(rescue).encode()) < 1000
    (OUT / 'cmdline-rescue').write_text(' '.join(rescue) + '\n')
    (OUT / 'entry-name-rescue').write_text(rescue_entry + '\n')

    payload = ROOT / f'build/boot/m1n1-linux-{entry}.bin'
    env = os.environ.copy()
    env.update(TREE='kexec-irqfix-usb4', M1N1=str(ROOT / 'build/m1n1-boot/m1n1.bin'),
               M1N1_VARS='psci=1', PAYLOAD_OUT=str(payload))
    assert tokens[:3] == ['earlycon', 'console=tty0', 'loglevel=7']
    initramfs = PREV_OUT / 'entry/initramfs.gz'
    run('bash', 'scripts/mkpayload.sh', str(initramfs), *tokens[3:], env=env)
    dest = OUT / 'entry'
    run('python3', '-B', '-I', 'scripts/boot/prepare-entry.py', str(payload), str(dest),
        '--board', 'apple,j71t')
    (dest / 'COMMIT').write_bytes((K / 'COMMIT').read_bytes())
    assert (dest / 'cmdline').read_text().strip() == cmdline
    image = gzip.decompress((K / 'Image.gz').read_bytes())
    assert gzip.decompress((dest / 'Image.gz').read_bytes()) == image
    assert gzip.decompress((dest / 'initramfs.gz').read_bytes()) == \
        gzip.decompress(initramfs.read_bytes())
    assert (dest / 'dtb').read_bytes() == (PREV_OUT / 'entry/dtb').read_bytes()
    for marker in (b'ans_keep_power', b'MSI resumed after vector drain',
                   b'drained every CQ on vector', b'apple_ans_irq_rescue_ms',
                   b'apple-tristar'):
        assert marker in image, marker
    files = sorted(p for p in dest.iterdir() if p.is_file() and p.name != 'SHA256SUMS')
    (dest / 'SHA256SUMS').write_text(''.join(f'{sha256(p)}  ./{p.name}\n' for p in files))
    manifest = dict(entry=entry, rescue_entry=rescue_entry, kernel_commit=commit,
                    parent_commit=prev['kernel_commit'], base_inputs=prev['base_inputs'],
                    hardware_tested=False, cmdline_bytes=len(cmdline.encode()),
                    config_change='none (usb-host4 config)',
                    payload=str(payload.relative_to(ROOT)), entry_dir=str(dest.relative_to(ROOT)),
                    files={str(p.relative_to(ROOT)): sha256(p)
                           for p in [payload, K / 'config', K / 'RELEASE', *files]})
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (OUT / 'entry-name').write_text(entry + '\n')
    (OUT / 'payload-path').write_text(str(payload.relative_to(ROOT)) + '\n')
    print(f'PASS offline payload: {entry} (rescue variant {rescue_entry}), '
          f'cmdline {len(cmdline.encode())} bytes')


if __name__ == '__main__':
    main()
