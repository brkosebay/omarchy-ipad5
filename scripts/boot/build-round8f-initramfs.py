#!/usr/bin/env python3
"""Add only the explicit ANS rescue retry to the exact saved Round 8 initramfs."""
import gzip
import hashlib
import importlib.machinery
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
merge = importlib.machinery.SourceFileLoader('merge', str(ROOT / 'scripts/merge-initramfs.py')).load_module()


def contents(data):
    return {merge.norm(n): (h, b) for n, h, b in merge.entries(data) if n != 'TRAILER!!!'}


def main():
    out = ROOT / 'build/boot/round8f'
    out.mkdir(parents=True, exist_ok=True)
    original = ROOT / 'build/boot/round8/initramfs.gz'
    manifest = json.loads((ROOT / 'build/boot/round8/build.json').read_text())
    assert hashlib.sha256(original.read_bytes()).hexdigest() == manifest['files'][str(original.relative_to(ROOT))]['sha256']
    base = gzip.decompress(original.read_bytes())
    files = contents(base)
    hdr, init = files['init']
    anchor = b'  echo "Retrying the boot"\n'
    assert init.count(anchor) == 1
    init = init.replace(anchor, anchor + b'  . /linux-a9-ans-rescue-retry.sh\n  linux_a9_rescue_retry_ans\n')
    # Overlay new inode numbers; preserve all original archives byte for byte.
    def entry(name, body, ino):
        h = hdr[:54] + b'%08X' % len(body) + hdr[62:]
        return merge.newc(name, h, body, ino)
    helper = (ROOT / 'scripts/boot/device/ans-rescue-retry.sh').read_bytes()
    tail = entry('init', init, 0x7b80001) + entry('linux-a9-ans-rescue-retry.sh', helper, 0x7b80002)
    tail += merge.newc('TRAILER!!!', b'070701' + b'0' * 104, b'', 0)
    tail += b'\0' * (-len(tail) % 512)
    result = base + tail
    revised = contents(result)
    assert set(revised) == set(files) | {'linux-a9-ans-rescue-retry.sh'}
    assert [n for n in files if revised[n][1] != files[n][1]] == ['init']
    assert revised['init'][1] == init and revised['linux-a9-ans-rescue-retry.sh'][1] == helper
    (out / 'init').write_bytes(init)
    (out / 'initramfs.gz').write_bytes(gzip.compress(result, compresslevel=9, mtime=0))
    print('PASS exact Round 8 archives preserved; only /init continue hook + rescue helper overlaid')


if __name__ == '__main__':
    main()
