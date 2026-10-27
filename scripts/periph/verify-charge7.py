#!/usr/bin/env python3
"""Read-only offline verification of the delivered charge7 manifest and entry."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    manifest = json.loads((ROOT / 'build/charge7/manifest.json').read_text())
    for name, digest in manifest['files'].items():
        assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest, name
    entry = ROOT / manifest['entry_dir']
    for line in (entry / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ./')
        assert hashlib.sha256((entry / name).read_bytes()).hexdigest() == digest, name
    tokens = (entry / 'cmdline').read_text().split()
    previous = (ROOT / 'build/usb-host5b/entry/cmdline').read_text().split()
    assert tokens.count('pcie_apple_h9p.ans_keep_power=1') == 1
    assert sum(t.startswith('pcie_apple_h9p.ans_keep_power=') for t in tokens) == 1
    assert not any(t.startswith('systemd.debug_shell') for t in tokens)
    assert f"linux_a9.entry={manifest['entry']}" in tokens
    assert [t for t in tokens if not t.startswith('linux_a9.entry=')] == [
        t for t in previous if not t.startswith('linux_a9.entry=')]
    assert not (ROOT / 'build/kernel-charge/DIRTY').read_text().strip()
    assert (ROOT / 'build/kernel-charge/COMMIT').read_text().split()[0] == manifest['kernel_commit']
    assert 'CONFIG_DEBUG_INFO_NONE=y' in (ROOT / 'build/kernel-charge/config').read_text().splitlines()
    assert manifest['hardware_tested'] is False
    assert manifest['voltage_adaptive_backoff'] is False
    print(f"PASS {manifest['entry']}: {len(manifest['files'])} manifest hashes, entry hashes, clean build, command line, debug info off")
    print('NOT RUN: device tests; UNRESOLVED: VBUS-triggered weak-source protection and sub-poll detach guarantee')


if __name__ == '__main__':
    main()
