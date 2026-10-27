#!/usr/bin/env python3
"""Verify round-8 source scope and packaged bytes, offline and read-only."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[2]
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def sources():
    unit = ROOT / 'scripts/omarchy/rootfs/files/etc/systemd/system/linux-a9-charger-limit.service'
    rules = ROOT / 'scripts/omarchy/rootfs/files/etc/udev/rules.d/90-linux-a9-charger.rules'
    assert 'ExecStart=/usr/bin/true' in unit.read_text()
    assert not any(l.strip() and not l.startswith('#') for l in rules.read_text().splitlines())
    init = (ROOT / 'scripts/omarchy/initramfs/init').read_text()
    assert 'charger_limit() { :; }' in init and '2400000' not in init
    config = (ROOT / 'scripts/omarchy/rootfs/in-container.sh').read_text()
    assert not any('systemctl enable' in l and 'linux-a9-charger-limit' in l for l in config.splitlines())
    ts = (ROOT / 'scripts/periph/kernel/apple_tristar_power.c').read_text()
    assert ts.count('regmap_write(ts->pmic, D2365_ADC_START, 1)') == 1
    assert ts.count('regmap_write(') == 1 and 'regmap_update_bits(' not in ts
    assert ts.count('ts->adc_poisoned = true') == 1
    assert 'ts->adc_poisoned = false' not in ts
    print('PASS source scope: automatic userspace requests retired; Tristar only writes guarded ADC trigger; poison never cleared')

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sources-only', action='store_true')
    args = ap.parse_args()
    sources()
    if args.sources_only:
        return
    manifest = json.loads((ROOT / 'build/charge8/manifest.json').read_text())
    for name, digest in manifest['files'].items():
        assert sha(ROOT / name) == digest, name
    entry = ROOT / manifest['entry_dir']
    for line in (entry / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ./')
        assert sha(entry / name) == digest, name
    tokens = (entry / 'cmdline').read_text().split()
    previous = (ROOT / 'build/charge7/entry/cmdline').read_text().split()
    assert tokens.count('pcie_apple_h9p.ans_keep_power=1') == 1
    assert sum(t.startswith('pcie_apple_h9p.ans_keep_power=') for t in tokens) == 1
    assert not any(t.startswith('systemd.debug_shell') for t in tokens)
    assert f"linux_a9.entry={manifest['entry']}" in tokens
    assert [t for t in tokens if not t.startswith('linux_a9.entry=')] == [
        t for t in previous if not t.startswith('linux_a9.entry=')]
    assert not (ROOT / 'build/kernel-charge/DIRTY').read_text().strip()
    assert (ROOT / 'build/kernel-charge/COMMIT').read_text().split()[0] == manifest['kernel_commit']
    assert (ROOT / 'build/kernel-charge/config').read_bytes() == (ROOT / 'build/kernel-usb-host5b/config').read_bytes()
    assert manifest['hardware_tested'] is False
    merge = runpy.run_path(str(ROOT / 'scripts/merge-initramfs.py'))
    old = list(merge['entries'](gzip.decompress((ROOT / 'build/charge7/entry/initramfs.gz').read_bytes())))
    new = list(merge['entries'](gzip.decompress((entry / 'initramfs.gz').read_bytes())))
    assert len(old) == len(new)
    changed = 0
    for (on,oh,ob), (nn,nh,nb) in zip(old,new):
        assert on == nn and oh[:54]+oh[62:] == nh[:54]+nh[62:]
        if merge['norm'](on) == 'init':
            assert b'charger_limit() { :; }' in nb and b'echo 2400000' not in nb
            changed += 1
        else:
            assert ob == nb, on
    assert changed
    print(f"PASS {manifest['entry']}: {len(manifest['files'])} manifest hashes; exact command line apart from entry; clean debug-info-off build; {changed} retired init records, all other archive contents/metadata unchanged")
    print('NOT RUN: hardware charging, warm-kexec and cable tests. UNRESOLVED: VBUS droop protection and sub-poll detach guarantee.')
if __name__ == '__main__':
    main()
