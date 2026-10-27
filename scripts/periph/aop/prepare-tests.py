#!/usr/bin/env python3
"""Host-only: package an integrated AOP build: passive/attach cmdlines and
payload (round 2) plus, with --active, the active-driver cmdline/payload
(active=1 attach=0), and with --attach the opt-in Round 4 running attach
(active=1 attach=1). --attach also prepares the active-only payload.

Preserves the known-good baseline's boot arguments. Never stages, connects to
the device or changes its default entry. Do not use the standalone aop build:
it lacks the integration tree's NVMe/kexec and recovery protections.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tree', default='integ')
    ap.add_argument('--base-cmdline', type=Path, default=ROOT / 'build/boot/integ-entry/cmdline')
    ap.add_argument('--active', action='store_true', help='also package the active driver (round 3)')
    ap.add_argument('--attach', action='store_true', help='also package the one-shot running attach (round 4; implies --active)')
    ap.add_argument('--round', type=int, choices=(4, 5, 6, 7), default=4,
                    help='active payload/output round (default 4; 7 requires a stable pinned remap)')
    args = ap.parse_args()
    args.active = args.active or args.attach
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.tree) or args.tree == 'aop':
        raise SystemExit('Select a full integration build tree, not standalone aop')
    kernel = ROOT / f'build/kernel-{args.tree}'
    if (kernel / 'DIRTY').read_text().strip():
        raise SystemExit('Refusing a dirty integration build; commit and rebuild first')
    config = (kernel / 'config').read_text().splitlines()
    required = ['APPLE_A9_AOP', 'APPLE_A9_AOP_IIO', 'DEBUG_FS', 'KEXEC_FILE', 'NVME_PCI_WRITE_WINDOW']
    if args.active:
        required.append('APPLE_A9_AOP_ACTIVE')
    for key in required:
        if f'CONFIG_{key}=y' not in config:
            raise SystemExit(f'Missing integration requirement CONFIG_{key}=y')
    baseline = shlex.split(args.base_cmdline.read_text())
    if any(any(c.isspace() for c in arg) for arg in baseline):
        raise SystemExit('Baseline contains quoted whitespace; preserve it with the normal boot workflow')
    effective = {arg.split('=', 1)[0]: arg for arg in baseline}
    for required in ('apple_pmgr.kexec_keep_on=1', 'panic=0', 'apple_wdt.cold_restart=0'):
        if effective.get(required.split('=', 1)[0]) != required:
            raise SystemExit(f'Baseline missing protective argument: {required}')
    baseline = [a for a in baseline if not a.startswith(('linux_a9.entry=', 'apple_a9_aop.debug=', 'apple_a9_aop.attach=',
                                                        'apple_a9_aop.active='))]
    commit = (kernel / 'COMMIT').read_text().split()[0]
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise SystemExit('Invalid build commit')
    out = ROOT / 'build/periph/aop-r2'
    out.mkdir(parents=True, exist_ok=True)
    passive = f'aop-r2-passive-{commit[:10]}'
    attach = f'aop-r2-attach-{commit[:10]}'
    cmdline = baseline + [f'linux_a9.entry={passive}', 'apple_a9_aop.debug=1',
                          'apple_a9_aop.active=0', 'apple_a9_aop.attach=0']
    (out / 'passive.cmdline').write_text(' '.join(cmdline) + '\n')
    active = baseline + [f'linux_a9.entry={attach}', 'apple_a9_aop.debug=1',
                         'apple_a9_aop.active=0', 'apple_a9_aop.attach=1']
    (out / 'attach.cmdline').write_text(' '.join(active) + '\n')
    payload = out / 'm1n1-aop-passive.bin'
    env = dict(os.environ, TREE=args.tree, PAYLOAD_OUT=str(payload),
               M1N1=str(ROOT / 'build/m1n1-boot/m1n1.bin'), M1N1_VARS='psci=1')
    subprocess.run([str(ROOT / 'scripts/mkpayload.sh'),
                    str(ROOT / 'build/boot/initramfs-omarchy.gz'), *cmdline], env=env, check=True)
    (out / 'entries.env').write_text(f'AOP_TREE={args.tree}\nAOP_PASSIVE={passive}\nAOP_ATTACH={attach}\n')
    print(f'Prepared only: {payload}\nPassive: {passive}\nObserver preflight only (active=0): {attach}')
    if not args.active:
        return
    active_out = ROOT / f'build/periph/aop-r{args.round}'
    active_out.mkdir(parents=True, exist_ok=True)
    name = f'aop-r{args.round}-active-{commit[:10]}'
    line = baseline + [f'linux_a9.entry={name}', 'apple_a9_aop.debug=1', 'apple_a9_aop.attach=0',
                       'apple_a9_aop.active=1']
    (active_out / 'active.cmdline').write_text(' '.join(line) + '\n')
    payload = active_out / 'm1n1-aop-active.bin'
    env = dict(env, PAYLOAD_OUT=str(payload))
    subprocess.run([str(ROOT / 'scripts/mkpayload.sh'),
                    str(ROOT / 'build/boot/initramfs-omarchy.gz'), *line], env=env, check=True)
    (active_out / 'entries.env').write_text(f'AOP_TREE={args.tree}\nAOP_ACTIVE={name}\n')
    print(f'Active, running preflight/ping only: {payload}\nActive entry: {name}')
    if args.attach:
        name = f'aop-r{args.round}-attach-{commit[:10]}'
        line = baseline + [f'linux_a9.entry={name}', 'apple_a9_aop.debug=1',
                           'apple_a9_aop.active=1', 'apple_a9_aop.attach=1']
        (active_out / 'attach.cmdline').write_text(' '.join(line) + '\n')
        payload = active_out / 'm1n1-aop-attach.bin'
        env = dict(env, PAYLOAD_OUT=str(payload))
        subprocess.run([str(ROOT / 'scripts/mkpayload.sh'),
                        str(ROOT / 'build/boot/initramfs-omarchy.gz'), *line], env=env, check=True)
        with (active_out / 'entries.env').open('a') as entries:
            entries.write(f'AOP_ATTACH={name}\n')
        print(f'Opt-in running attach: {payload}\nAttach entry: {name}')


if __name__ == '__main__':
    main()
