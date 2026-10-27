#!/usr/bin/env python3
"""Replay the mail series in a private Git index; never check out/reset a tree."""
import os
from pathlib import Path
import subprocess
import tempfile

BASE = '6831bc701'


def main():
    folder = Path('/work/patches/periph')
    names = [p.name for p in sorted(folder.glob('*.patch'))]
    assert names, 'no exported patches'
    (folder / 'series').write_text('\n'.join(names) + '\n')
    with tempfile.TemporaryDirectory(dir='/work/build/periph', prefix='patch-check-') as temp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], env=env, check=True)
        for name in names:
            subprocess.run(['git', 'apply', '--cached', '--whitespace=error', str(folder / name)], env=env, check=True)
        replayed = subprocess.check_output(['git', 'write-tree'], env=env, text=True).strip()
        expected = subprocess.check_output(['git', 'rev-parse', 'HEAD^{tree}'], text=True).strip()
        assert replayed == expected, f'patch result differs: {replayed} != {expected}'
    print(f'PASS: {len(names)} patches replay from {BASE} to {expected} with a private index')


if __name__ == '__main__':
    main()
