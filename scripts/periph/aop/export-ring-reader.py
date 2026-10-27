#!/usr/bin/env python3
"""Export only committed ring-reader kernel changes; run in Docker tree aop."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

BASE = 'b0fbdf663609d05c1523a4d61f631eea43867471'


def git(*args):
    return subprocess.check_output(['git', *args], text=True).strip()


def main():
    if Path.cwd() != Path('/src/wt/aop') or git('branch', '--show-current') != 'a9/aop':
        raise SystemExit('Run with scripts/build-kernel.sh --run aop')
    if git('status', '--porcelain'):
        raise SystemExit('Refusing to export a dirty kernel tree')
    subprocess.run(['git', 'merge-base', '--is-ancestor', BASE, 'HEAD'], check=True)
    if git('rev-parse', 'HEAD') == BASE:
        raise SystemExit('No ring-reader commits to export')
    out = Path('/work/patches/aop/ring-reader')
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='aop-ring-reader-') as temp:
        names = [Path(name) for name in git('format-patch', '--no-signature',
                 '--output-directory', temp, f'{BASE}..HEAD').splitlines()]
        # Replay into an isolated index; no worktree or branch/ref is changed.
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], env=env, check=True)
        subprocess.run(['git', 'apply', '--cached', *map(str, names)], env=env, check=True)
        replay = subprocess.check_output(['git', 'write-tree'], env=env, text=True).strip()
        if replay != git('rev-parse', 'HEAD^{tree}'):
            raise SystemExit('Exported patches do not reconstruct the HEAD tree')
        expected = {name.name for name in names}
        extra = {path.name for path in out.glob('*.patch')} - expected
        if extra:
            raise SystemExit(f'Refusing to leave stale patches: {sorted(extra)}')
        for name in names:
            shutil.copyfile(name, out / name.name)
        (out / 'series').write_text(''.join(f'{name.name}\n' for name in names))
        sums = [f'{hashlib.sha256((out / name.name).read_bytes()).hexdigest()}  {name.name}\n'
                for name in names]
        sums.append(f'{hashlib.sha256((out / "series").read_bytes()).hexdigest()}  series\n')
        (out / 'SHA256SUMS').write_text(''.join(sums))
    print(f'Exported/replayed {len(names)} patches, series and SHA256SUMS: {git("rev-parse", "HEAD")}')


if __name__ == '__main__':
    main()
