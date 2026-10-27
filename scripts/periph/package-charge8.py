#!/usr/bin/env python3
"""Offline charge8 payload/entry, preserving charge7 command line, retiring automatic initrd cap."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/charge8'
KERNEL = ROOT / 'build/kernel-charge'
BASE = ROOT / 'build/charge7'
PARENT = 'a8aa6fea363394f3d21f2eef7525998884b4cf3f'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, **kwargs):
    subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def retire_initrd():
    # Preserve every archive/file except the old request block in /init.
    # Rewrite its newc file size in place; appending a second executable /init
    # can collide with initramfs hardlinks, so modify the existing archive.
    import importlib.util
    spec = importlib.util.spec_from_file_location('merge', ROOT / 'scripts/merge-initramfs.py')
    merge = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(merge)
    data = gzip.decompress((BASE / 'entry/initramfs.gz').read_bytes())
    out = bytearray()
    found = 0
    for name, header, body in merge.entries(data):
        if merge.norm(name) == 'init':
            text = body.decode()
            start = text.index('# The j71t charger draws from the Mac only')
            end = text.index('\n# Root shells', start)
            text = text[:start] + '# Kernel derives the board ceiling; explicit sysfs writes are user caps.\ncharger_limit() { :; }\n' + text[end:]
            body = text.encode()
            found += 1
        header = header[:54] + f'{len(body):08x}'.encode() + header[62:]
        record = header + name.encode() + b'\0'
        record += b'\0' * (-len(record) % 4)
        record += body + b'\0' * (-len(body) % 4)
        out.extend(record)
    assert found >= 1
    out.extend(b'\0' * (-len(out) % 512))
    # Repacking must preserve contents/modes/linkage for every other file.
    old = [(n,h[:54]+h[62:],b) for n,h,b in merge.entries(data) if merge.norm(n) != 'init']
    new = [(n,h[:54]+h[62:],b) for n,h,b in merge.entries(out) if merge.norm(n) != 'init']
    assert old == new
    (OUT / 'initramfs.gz').write_bytes(gzip.compress(out, mtime=0))


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
    entry = 'charge8-' + commit[:10]
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
    retire_initrd()
    payload = OUT / f'm1n1-linux-{entry}.bin'
    env = dict(os.environ, TREE='charge', M1N1=str(ROOT / 'build/m1n1-boot/m1n1.bin'),
               M1N1_VARS='psci=1', PAYLOAD_OUT=str(payload))
    run('bash', 'scripts/mkpayload.sh', str(OUT / 'initramfs.gz'), *tokens[3:], env=env)
    dest = OUT / 'entry'
    run('python3', '-B', '-I', 'scripts/boot/prepare-entry.py', str(payload), str(dest),
        '--board', 'apple,j71t')
    (dest / 'COMMIT').write_bytes((KERNEL / 'COMMIT').read_bytes())
    assert (dest / 'cmdline').read_text() == cmdline
    image = gzip.decompress((KERNEL / 'Image.gz').read_bytes())
    assert gzip.decompress((dest / 'Image.gz').read_bytes()) == image
    assert gzip.decompress((dest / 'initramfs.gz').read_bytes()) == gzip.decompress(
        (OUT / 'initramfs.gz').read_bytes())
    assert (dest / 'dtb').read_bytes() == (KERNEL / 'dtbs/s8003-j71t.dtb').read_bytes()
    for marker in (b'charge7_state', b'charger_detect:', b'cc-debounce', b'pmic-input-reduced', b'vbus_mv=unavailable',
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
                    pmu_input_reduction_backoff=True, config_change='none (charge7 config)', initramfs_change='retire automatic 2.4A write',
                    payload=str(payload.relative_to(ROOT)), payload_sha256=sha(payload),
                    entry_dir=str(dest.relative_to(ROOT)),
                    files={str(p.relative_to(ROOT)): sha(p) for p in artifacts})
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (OUT / 'entry-name').write_text(entry + '\n')
    (OUT / 'payload-path').write_text(str(payload.relative_to(ROOT)) + '\n')
    # mkpayload's deterministic temporary copy is not another delivered payload.
    (ROOT / f'build/.initrd-single-{payload.stem}.gz').unlink(missing_ok=True)
    print(f'PASS {entry}: payload sha256 {sha(payload)}; unchanged cmdline; retired initrd request; new DTBs')


if __name__ == '__main__':
    main()
