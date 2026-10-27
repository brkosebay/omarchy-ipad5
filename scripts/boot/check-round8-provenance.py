#!/usr/bin/env python3
"""Audit the cached Round 8 entries and original detached source, never a device."""
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    out = ROOT / 'build/boot/round8b'
    out.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([str(ROOT / 'scripts/build-kernel.sh'), '--run', 'boot', '''
git diff --binary b48c325d1a1dc8c10f821a6b8dff9811cb1f6d40 18eecae442 > /tmp/round8-expected.patch
cmp /tmp/round8-expected.patch /work/build/boot/round8/kernel.patch
git -C /src/wt/bootr8-check diff --binary > /tmp/round8-actual.patch
cmp /tmp/round8-actual.patch /work/build/boot/round8/kernel.patch
echo "PASS round8 saved overlay equals intended commits and original detached source"
'''], capture_output=True, text=True)
    (out / 'provenance.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    kernel = ROOT / 'build/kernel-bootr8-check'
    original = ROOT / 'build/boot/round8'
    image = gzip.decompress((kernel / 'Image.gz').read_bytes())
    release = re.search(rb'Linux version (\S+)', image)[1].decode()
    base = (original / 'integration-base.txt').read_text().strip()
    head = (original / 'boot-head.txt').read_text().strip()
    assert head.startswith('18eecae442') and base.startswith('d5822a7f71cd')
    assert (kernel / 'COMMIT').read_text().startswith(base)
    assert release == '7.3.0-rc1-gd5822a7f71cd-dirty'
    original_manifest = json.loads((original / 'build.json').read_text())
    image_hash = sha(kernel / 'Image.gz')
    assert image_hash == original_manifest['files']['build/kernel-bootr8-check/Image.gz']['sha256']
    for mode in ('handoff', 'full-cycle', 'round7'):
        entry = original / f'entry-{mode}'
        assert sha(entry / 'Image.gz') == image_hash
        assert (entry / 'COMMIT').read_text().startswith(head)
    for marker in (b'DART checked reinit:', b'health gate partition/data NSID 1',
                   b'handoff already-enabled L0'):
        assert marker in image, marker
    record = {
        'kernel_head': head, 'integration_base': base, 'kernel_release': release,
        'image_sha256': image_hash, 'overlay_sha256': sha(original / 'kernel.patch'),
        'source_overlay_matches_intended_commits': True,
        'all_entries_match_built_image': True, 'round8_markers_present': True,
        'version_explanation': 'Detached integration plus an uncommitted exact overlay; expected base + dirty suffix.',
    }
    (out / 'round8-provenance.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS Round 8 intended overlay and entry image:', release, image_hash)


if __name__ == '__main__':
    main()
