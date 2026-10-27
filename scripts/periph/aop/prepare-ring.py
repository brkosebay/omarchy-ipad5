#!/usr/bin/env python3
"""Host only: prepare scan/target/E2 cmdline and DTB, never stage or boot.

E2 requires an explicit PA from the orchestrator's E1 evidence. The output
reserves the covering 16 KiB kernel pages before allocation on the NEXT boot.
It cannot protect a ring in the current unreserved kernel or across iOS boots.
"""
import argparse
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OBSERVER = '/soc/aop-observer@210800044'
PAGE_SIZE = 16384
RING_SIZE = 16384


def fdtget(dtb, node, prop=None, kind='x', required=True):
    args = ['fdtget', '-t', kind, str(dtb), node, prop] if prop else [
        'fdtget', '-l', str(dtb), node]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        if required:
            raise ValueError(result.stderr.strip())
        return None
    return result.stdout.split()


def reserve(dtb, pa):
    if pa % 64 or pa <= 0 or pa >= (1 << 48) - RING_SIZE:
        raise ValueError('Ring PA must be positive, 64-byte aligned and below 48 bits')
    if fdtget(dtb, OBSERVER, 'memory-region', required=False) is not None:
        raise ValueError('Input DTB already has a ring hint; use the pristine build DTB')
    for node in ('/', '/reserved-memory'):
        for prop in ('#address-cells', '#size-cells'):
            if fdtget(dtb, node, prop) != ['2']:
                raise ValueError(f'{node} requires two {prop}')
    base = pa & ~(PAGE_SIZE - 1)
    offset = pa - base
    size = (offset + RING_SIZE + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)
    for name in fdtget(dtb, '/reserved-memory'):
        values = fdtget(dtb, f'/reserved-memory/{name}', 'reg', required=False)
        if values is None:
            continue
        cells = [int(v, 16) for v in values]
        if len(cells) % 4:
            raise ValueError(f'Malformed reservation {name}')
        for i in range(0, len(cells), 4):
            other = cells[i] << 32 | cells[i + 1]
            length = cells[i + 2] << 32 | cells[i + 3]
            if length and other < base + size and base < other + length:
                raise ValueError(f'Ring overlaps existing reservation {name}')
    text = subprocess.check_output(['dtc', '-q', '-I', 'dtb', '-O', 'dts', str(dtb)], text=True)
    used = [int(v, 16) for v in re.findall(r'\bphandle\s*=\s*<0x([0-9a-fA-F]+)>', text)]
    phandle = max(used, default=0) + 1
    if phandle >= 0xffffffff:
        raise ValueError('No free phandle')
    node = f'/reserved-memory/aop-inherited-ring@{base:x}'
    subprocess.run(['fdtput', '-c', str(dtb), node], check=True)

    def put(node, prop, values, kind='x'):
        subprocess.run(['fdtput', '-t', kind, str(dtb), node, prop, *values], check=True)

    put(node, 'reg', [f'{base >> 32:x}', f'{base & 0xffffffff:x}', '0', f'{size:x}'])
    put(node, 'phandle', [f'{phandle:x}'])
    subprocess.run(['fdtput', str(dtb), node, 'no-map'], check=True)
    put(OBSERVER, 'memory-region', [f'{phandle:x}'])
    put(OBSERVER, 'apple,ring-offset', [f'{offset:x}'])
    return f'''/* Orchestrator-selected PA 0x{pa:x}; for the next boot only.
 * Keep every other inherited DMA buffer reserved too. This is one ring.
 * No compatible, reusable, DMA pool or second consumer is permitted.
 */
/ {{
    reserved-memory {{
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;
        aop_inherited_ring: aop-inherited-ring@{base:x} {{
            reg = <0x{base >> 32:x} 0x{base & 0xffffffff:x} 0 0x{size:x}>;
            no-map;
        }};
    }};
}};
&{{{OBSERVER}}} {{
    memory-region = <&aop_inherited_ring>;
    apple,ring-offset = <0x{offset:x}>;
}};
'''


def cmdline(baseline, entry, mode, early=False, reserved=False, buffers=()):
    if mode not in ('scan', 'target', 'iio'):
        raise ValueError('Unknown ring mode')
    if mode == 'target':
        if not 1 <= len(buffers) <= 4:
            raise ValueError('Target mode requires one to four explicit PA:size buffers')
        for pa, size in buffers:
            if not 0 < pa < 1 << 64 or not 1 <= size <= 65536 or size > (1 << 64) - 1 - pa:
                raise ValueError('Target PA/size is invalid or exceeds 64 KiB')
    elif buffers:
        raise ValueError('Explicit buffers require target mode')
    if early and mode != 'scan':
        raise ValueError('Early capture is only available in scan mode')
    if reserved and mode != 'scan':
        raise ValueError('Reserved DRAM discovery is only available in scan mode')
    args = shlex.split(baseline)
    if any(any(c.isspace() for c in a) for a in args):
        raise ValueError('Quoted whitespace in baseline requires the normal boot workflow')
    effective = {a.split('=', 1)[0]: a for a in args}
    for required in ('apple_pmgr.kexec_keep_on=1', 'panic=0', 'apple_wdt.cold_restart=0'):
        if effective.get(required.split('=', 1)[0]) != required:
            raise ValueError(f'Baseline missing protective argument {required}')
    args = [a for a in args if not a.startswith(('apple_a9_aop.', 'linux_a9.entry='))]
    args += [f'linux_a9.entry={entry}', 'apple_a9_aop.debug=1',
             'apple_a9_aop.active=0', 'apple_a9_aop.attach=0',
             f'apple_a9_aop.ring_scan={int(mode == "scan")}',
             f'apple_a9_aop.ring_scan_early={int(early)}',
             f'apple_a9_aop.ring_scan_reserved={int(reserved)}',
             f'apple_a9_aop.ring_iio={int(mode == "iio")}']
    if mode == 'target':
        args += ['apple_a9_aop.ring_pa=' + ','.join(f'0x{pa:x}' for pa, _ in buffers),
                 'apple_a9_aop.ring_pa_size=' + ','.join(f'0x{size:x}' for _, size in buffers)]
    return ' '.join(args) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tree', required=True)
    ap.add_argument('--mode', choices=('scan', 'target', 'iio'), required=True)
    ap.add_argument('--early', action='store_true', help='capture E1 at postcore initcall')
    ap.add_argument('--reserved', action='store_true', help='also scan reserved linear-mapped DRAM')
    ap.add_argument('--ring-pa', type=lambda x: int(x, 0))
    ap.add_argument('--buffer', action='append', default=[], metavar='PA:SIZE',
                    help='exact iOS-probed range for target mode; repeat up to four times')
    ap.add_argument('--base-cmdline', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.tree) or args.tree == 'aop':
        ap.error('Use a full integration tree, not standalone aop')
    if (args.mode == 'iio') != (args.ring_pa is not None):
        ap.error('Only IIO mode requires --ring-pa from E1 evidence')
    if args.early and args.mode != 'scan':
        ap.error('--early requires --mode scan')
    if args.reserved and args.mode != 'scan':
        ap.error('--reserved requires --mode scan')
    try:
        buffers = [tuple(int(v, 0) for v in pair.split(':')) for pair in args.buffer]
        if any(len(pair) != 2 for pair in buffers):
            raise ValueError('Expected PA:SIZE')
    except ValueError as error:
        ap.error(f'Invalid --buffer: {error}')
    kernel = ROOT / f'build/kernel-{args.tree}'
    if (kernel / 'DIRTY').read_text().strip():
        raise ValueError('Integration build is dirty')
    config = (kernel / 'config').read_text().splitlines()
    for symbol in ('APPLE_A9_AOP', 'ARM64_16K_PAGES', 'OF_RESERVED_MEM',
                   'DEBUG_FS', 'KEXEC_FILE', 'NVME_PCI_WRITE_WINDOW') + (
                       ('IIO',) if args.mode == 'iio' else ()):
        if f'CONFIG_{symbol}=y' not in config:
            raise ValueError(f'Missing CONFIG_{symbol}=y')
    commit = (kernel / 'COMMIT').read_text().split()[0]
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Bad build commit')
    entry = (f'aop-ring-{args.mode}{"-early" if args.early else ""}'
             f'{"-reserved" if args.reserved else ""}-{commit[:10]}')
    line = cmdline(args.base_cmdline.read_text(), entry, args.mode, args.early, args.reserved, buffers)
    args.out.mkdir(parents=True, exist_ok=False)
    dtb = args.out / 'dtb'
    shutil.copyfile(kernel / 'dtbs/s8003-j71t.dtb', dtb)
    if args.mode == 'iio':
        (args.out / 'reservation.dtsi').write_text(reserve(dtb, args.ring_pa))
    (args.out / 'cmdline').write_text(line)
    (args.out / 'entry').write_text(entry + '\n')
    (args.out / 'COMMIT').write_text((kernel / 'COMMIT').read_text())
    print(f'Prepared {args.out}: entry {entry}; no staging, device access or boot')


if __name__ == '__main__':
    main()
