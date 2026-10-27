#!/usr/bin/env python3
"""Turn one pongoOS SEP capture into the tri53-sep2 xART retest commands.

Usage: python3 -B -I scripts/sep/xart-retest-plan.py PONGO_LOG [--out DIR]

Reads the `sep xnu` transcript of THIS sepOS boot (build/sep/pongo-sep.log)
and checks the gates of docs/sep/linux-live-test.md section 7: sepOS booted,
nothing overwritten in the capture ring, root shmem equal to tri53-sep2's
no-map reservation (0x87adb0000), consistent endpoint-0xfd pairs, exactly one
xarm request with op 0x0e, param 0, data 0. On success it prints the values
and, with --out, writes DIR/plan.env and DIR/remote-xart.sh: the debugfs
writes (declare this boot's advertisements, publish xarm OOL, one
capability-0 reply) for `scripts/omarchy/ssh.sh 'sudo -n sh -s' <remote-xart.sh`.

Exit status 0: go. 3: a gate failed (STOP, message on stderr). 2: usage.
Offline only; it never talks to the device.
"""
import re
import sys
from pathlib import Path

SHMEM_PA = 0x87adb0000      # sep-shmem@87adb0000 in the tri53-sep2 DT
SHMEM_PAGES = 1             # what the 13:27 boot handed over and was reserved
OOL_BYTES = 0x8000          # AppleSEPXART_initOOLBuffers: 32 KiB send and receive
XARM = 0x7861726d           # 'xarm'
XART_OP_CAPABILITY = 0x0e
PAGE = 0x1000

ANSI = re.compile(r'\x1b\[[0-9;]*m')
MSG = re.compile(r'sep capture: msg ep=([0-9a-f]+) tag=([0-9a-f]+) op=([0-9a-f]+) '
                 r'param=([0-9a-f]+) data=([0-9a-f]+) \(raw=([0-9a-f]+)\)')
SHMEM = re.compile(r'sep: -> root shmem ep=fe tag=([0-9a-f]+) pages=([0-9a-f]+) data=([0-9a-f]+)')
SUMMARY = re.compile(r'sep capture: total=(\d+) retained=(\d+) overwritten=(\d+)')


def stop(msg):
    print(f'STOP: {msg}', file=sys.stderr)
    sys.exit(3)


def fourcc(word):
    return word.to_bytes(4, 'big').decode('ascii', 'replace')


def parse(text):
    lines = [ANSI.sub('', l).rstrip('\r') for l in text.splitlines()]
    booted = sum('sep xnu: SEPOS BOOTED' in l for l in lines)
    if booted != 1:
        stop(f"expected one 'sep xnu: SEPOS BOOTED' line, found {booted} "
             '(one capture per log: use the log of this boot only)')

    shm = [m for l in lines if (m := SHMEM.search(l))]
    if len(shm) != 1:
        stop(f"expected one 'root shmem' line, found {len(shm)}")
    pages, pa = int(shm[0].group(2), 16), int(shm[0].group(3), 16) << 12
    if pa != SHMEM_PA or pages != SHMEM_PAGES:
        stop(f'root shmem is {pa:#x} ({pages} page(s)); tri53-sep2 reserves '
             f'{SHMEM_PA:#x} ({SHMEM_PAGES}). Do not kexec into tri53-sep2: its no-map '
             'reservation would protect the wrong page. Rebuild the DT for this address first.')

    summ = [m for l in lines if (m := SUMMARY.search(l))]
    if len(summ) != 1 or int(summ[0].group(3)) != 0:
        stop('capture summary missing or ring overwritten: the stream is incomplete')

    msgs = []
    for l in lines:
        m = MSG.search(l)
        if not m:
            continue
        ep, tag, op, param, data, raw = (int(g, 16) for g in m.groups())
        if raw != (data << 32 | param << 24 | op << 16 | tag << 8 | ep):
            stop(f'raw word does not match its fields: {l.strip()}')
        msgs.append((ep, tag, op, param, data, raw))
    if len(msgs) != int(summ[0].group(2)):
        stop(f'{len(msgs)} capture lines but retained={summ[0].group(2)}')

    names, oolw = {}, {}
    for ep, tag, op, param, data, raw in msgs:
        if ep != 0xfd:
            continue
        table = names if op == 0 else oolw if op == 1 else None
        if table is None:
            stop(f'unknown discovery opcode in raw {raw:016x}')
        if table.setdefault(param, data) != data:
            stop(f'conflicting advertisements for endpoint {param:#04x}')
    if set(names) != set(oolw):
        stop(f'incomplete advertisement pairs: names {sorted(names)} ool {sorted(oolw)}')

    xarm = [i for i, w in names.items() if w == XARM]
    if len(xarm) != 1:
        stop(f'expected one xarm advertisement, found {len(xarm)}')
    e = xarm[0]
    o = oolw[e]
    in_min, in_max, out_min, out_max = (o >> s & 0xff for s in (0, 8, 16, 24))
    if not (in_min * PAGE <= OOL_BYTES <= in_max * PAGE and
            out_min * PAGE <= OOL_BYTES <= out_max * PAGE):
        stop(f'xarm OOL ranges in [{in_min},{in_max}] out [{out_min},{out_max}] pages '
             f'do not contain {OOL_BYTES:#x}: not the protocol this plan was written for')

    reqs = [m for m in msgs if m[0] == e]
    if len(reqs) != 1:
        stop(f'expected exactly one xarm message, found {len(reqs)}: '
             + ' '.join(f'{m[5]:016x}' for m in reqs))
    _, t, op, param, data, raw = reqs[0]
    if (op, param, data) != (XART_OP_CAPABILITY, 0, 0):
        stop(f'xarm request {raw:016x} is not op 0x0e param 0 data 0: derive the reply from it, '
             'do not use this plan')
    others = [m for m in msgs if m[0] not in (0xfd, e)]
    return dict(e=e, t=t, raw=raw, names=names, oolw=oolw, others=others, pa=pa)


def main(argv):
    if len(argv) not in (1, 3) or (len(argv) == 3 and argv[1] != '--out'):
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    p = parse(Path(argv[0]).read_text(errors='replace'))
    e, t = p['e'], p['t']
    adv = [f'raw {i:02x} {p["names"][i]:08x} {p["oolw"][i]:08x}' for i in sorted(p['names'])]
    ool = f'{e:02x} {OOL_BYTES:x} {OOL_BYTES:x}'
    reply = f'{e:02x} {t:02x} 00 00 00000000'

    print(f'GO: root shmem {p["pa"]:#x} matches the reservation')
    print(f'xarm request raw={p["raw"]:016x}: E={e:#04x} T={t:#04x}')
    for i in sorted(p['names']):
        print(f'  ep {i:#04x} {fourcc(p["names"][i])} ool={p["oolw"][i]:08x}')
    for m in p['others']:
        print(f'  other: ep={m[0]:02x} tag={m[1]:02x} op={m[2]:02x} param={m[3]:02x} data={m[4]:08x}')
    print('advertise:\n  ' + '\n  '.join(adv))
    print(f'ool:   {ool}\nreply: {reply}')

    if len(argv) == 3:
        out = Path(argv[2])
        out.mkdir(parents=True, exist_ok=True)
        (out / 'plan.env').write_text(f'E={e:02x}\nT={t:02x}\nXARM_RAW={p["raw"]:016x}\n')
        script = ['# generated by scripts/sep/xart-retest-plan.py; run once, never again in this sepOS boot',
                  'set -u',
                  'D=/sys/kernel/debug/sep-a9',
                  'echo "== fifo before"; cat "$D/fifo"',
                  'grep -q "^ap->sep snd_sts=.* (empty)$" "$D/fifo" || { echo "STOP: AP->SEP FIFO not empty or not armed"; exit 1; }',
                  *[f'echo "{a}" >"$D/advertise" || {{ echo "STOP: advertise {a}"; exit 1; }}' for a in adv],
                  'echo "== endpoints"; cat "$D/endpoints"',
                  f'echo "== ool {ool}"',
                  f'if ! echo "{ool}" >"$D/ool"; then',
                  '\techo "STOP: ool failed (transport FAIL or refused); no reply sent. Cold reset."',
                  '\tcat "$D/fifo"; dmesg | grep sep-a9 | tail -8; exit 1',
                  'fi',
                  'cat "$D/fifo"',
                  f'echo "== reply {reply}"',
                  f'echo "{reply}" >"$D/send" || {{ echo "STOP: reply not queued"; exit 1; }}',
                  'cat "$D/fifo"; cat "$D/status"',
                  'dmesg | grep sep-a9 | tail -12']
        (out / 'remote-xart.sh').write_text('\n'.join(script) + '\n')
        print(f'wrote {out / "plan.env"} and {out / "remote-xart.sh"}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
