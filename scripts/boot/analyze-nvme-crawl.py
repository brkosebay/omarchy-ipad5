#!/usr/bin/env python3
"""Summarize ANS NVMe lost-interrupt evidence ("crawl") in saved dmesg logs.

Usage: analyze-nvme-crawl.py DMESG...   (offline; see docs/boot/ds-kexec-nvme-crawl.md)

Per log: NVMe-ready time, the first I/O and admin timeouts with the
submit window that nvme_core.io_timeout/admin_timeout imply, the nvme0q
IRQ count in the ANS queue dumps (frozen = a single value), how many
timeouts found a posted-but-unconsumed CQE in both CQs, and the first
stuck admin opcode. Lines replayed from earlier boots ([omarchy-rd]) are
ignored.
"""
import re
import sys

TS = r'^\[\s*([\d.]+)\]'
TIMEOUT = re.compile(TS + r'.*Apple ANS: timeout tag (\d+) cid (0x[0-9a-f]+) qid (\d+) opcode (0x[0-9a-f]+)')
QUEUE = re.compile(TS + r'.*Apple ANS: q(\d) flags \S+ depth \d+ SQ tail (\d+)/\d+ CQ head (\d+) '
                   r'phase (\d) next status (0x[0-9a-f]+) .* count (\d+)')
POLLED = re.compile(TS + r'.*I/O tag (-?\d+) \(\w+\) QID (\d+) timeout, completion polled')
READY = re.compile(TS + r'.*nvme nvme0: (\d+/\d+/\d+) default/read/poll queues')
PARAM = re.compile(r'nvme_core\.(io|admin)_timeout=(\d+)')


def analyze(lines):
    r = dict(ready=None, polled=0, first_io=None, first_admin=None, counts=set(),
             timeouts=0, both=0, io_timeout=30, admin_timeout=60)
    snap = None
    for line in lines:
        if 'omarchy-rd' in line:
            continue
        if 'Kernel command line' in line:
            for kind, value in PARAM.findall(line):
                r[kind + '_timeout'] = int(value)
        if m := READY.match(line):
            r['ready'] = r['ready'] or float(m[1])
        elif m := POLLED.match(line):
            r['polled'] += 1
        elif m := TIMEOUT.match(line):
            t, qid, op = float(m[1]), int(m[4]), m[5]
            key = 'first_admin' if qid == 0 else 'first_io'
            r[key] = r[key] or (t, op)
            r['timeouts'] += 1
            snap = {}
        elif (m := QUEUE.match(line)) and snap is not None:
            qid, phase, status, count = int(m[2]), int(m[5]), int(m[6], 16), int(m[7])
            snap[qid] = (status & 1) == phase
            r['counts'].add(count)
            if len(snap) == 2:
                r['both'] += snap[0] and snap[1]
                snap = None
    return r


def main(paths):
    for path in paths:
        with open(path, errors='replace') as f:
            r = analyze(f)
        name = path.rsplit('/', 1)[-1][:48]
        if not r['polled']:
            print(f'{name}: no completion-polled timeouts (NVMe ready {r["ready"]})')
            continue
        out = [f'{name}: {r["polled"]} polled', f'NVMe ready {r["ready"]} s']
        if r['first_io']:
            t = r['first_io'][0]
            out.append(f'first I/O timeout {t:.2f} (submitted in ({t - r["io_timeout"] - 1:.1f}, '
                       f'{t - r["io_timeout"]:.1f}])')
        if r['first_admin']:
            t, op = r['first_admin']
            out.append(f'first admin timeout {t:.2f} opcode {op} (submitted in '
                       f'({t - r["admin_timeout"] - 1:.1f}, {t - r["admin_timeout"]:.1f}])')
        if r['counts']:
            counts = sorted(r['counts'])
            out.append('IRQ count ' + (f'frozen at {counts[0]}' if len(counts) == 1 else
                                       f'{counts[0]}..{counts[-1]}'))
            out.append(f'both CQs pending at {r["both"]}/{r["timeouts"]} timeouts')
        print('; '.join(out))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip())
    main(sys.argv[1:])
