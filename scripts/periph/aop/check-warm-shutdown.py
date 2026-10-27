#!/usr/bin/env python3
"""Check a retained previous-kernel QUIESCE transcript, offline/read-only.

Input is collect-hang-evidence.sh's output directory, one console file, the
new kernel's dmesg (AOP-PREV lines replayed from the reserved transcript) or
debugfs previous_shutdown. Checks the logged transition, not reset/streaming.
"""
import argparse
from pathlib import Path
import re


def check(text, entry):
    start = text.rfind('QUIESCE step=begin')
    assert start >= 0, 'no shutdown begin'
    begin = text.rfind('SHUTDOWN begin mode=quiesce', 0, start)
    if begin >= 0:
        line = text[begin:].splitlines()[0]
        assert re.search(rf'entry={re.escape(entry)}(\s|$)', line), 'previous entry not identified'
    else:
        assert f'linux_a9.entry={entry}' in text[:start], 'previous entry not identified'
    trace = text[start:]
    assert not re.search(r'QUIESCE (failed|step=(skipped|ap-timeout|iop-timeout|afk-(timeout|budget|unstopped)))',
                         trace), 'failed transition'
    tokens = [
        'QUIESCE step=rates-zero status=0',
        'QUIESCE step=ap-send word=00b0000000000010 status=0',
        'QUIESCE step=ap-ack state=0x10',
        'QUIESCE step=iop-send word=0060000000000201 status=0',
        'QUIESCE step=iop-ack state=0x201',
        'QUIESCE step=ram-stable',
        'QUIESCE step=halt status=0',
        'QUIESCE step=drain status=0',
        'QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1',
        'STAGE3 checkpoint=shutdown-quiesce-complete',
    ]
    last = -1
    for token in tokens:
        at = trace.find(token)
        assert at > last, f'missing/out-of-order: {token}'
        last = at
    stable = next(line for line in trace.splitlines() if 'QUIESCE step=ram-stable' in line)
    assert re.search(r'window_ms=20 before=([0-9a-f]{64}) after=\1 status=0', stable), 'RAM stability failed'
    sent = re.findall(r'QUIESCE step=afk-send ep=(0x[0-9a-f]+) status=0', trace)
    assert len(sent) == len(set(sent)), 'duplicate AFK shutdown'
    for ep in sent:
        ack = trace.find(f'QUIESCE step=afk-ack ep={ep} state=stopped')
        disable = trace.find(f'QUIESCE step=endpoint-disable ep={ep}')
        assert ack >= 0 and ack < disable < trace.find(tokens[1]), f'AFK ACK/disable missing: {ep}'
    for line in trace.splitlines():
        if 'QUIESCE step=afk-send' in line or 'QUIESCE step=endpoint-disable' in line:
            assert line.endswith('status=0'), line
    return len(sent)


def self_test():
    digest = 'ab' * 32
    good = '\n'.join((
        'Kernel command line: linux_a9.entry=fixture', 'QUIESCE step=begin',
        'QUIESCE step=rates-zero status=0 accel=0',
        'QUIESCE step=afk-send ep=0x21 status=0',
        'QUIESCE step=afk-ack ep=0x21 state=stopped',
        'QUIESCE step=endpoint-disable ep=0x21 word=0050002100000001 status=0',
        'QUIESCE step=ap-send word=00b0000000000010 status=0',
        'QUIESCE step=ap-ack state=0x10',
        'QUIESCE step=iop-send word=0060000000000201 status=0',
        'QUIESCE step=iop-ack state=0x201',
        f'QUIESCE step=ram-stable bytes=0x46000 window_ms=20 before={digest} after={digest} status=0',
        'QUIESCE step=halt status=0 cpu44=00000000',
        'QUIESCE step=drain status=0 inbox=0 outbox=0 pool=retained',
        'QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1',
        'STAGE3 checkpoint=shutdown-quiesce-complete',
    ))
    assert check(good, 'fixture') == 1
    # a9/aop-wake4 appends the ACK latency and words received while waiting.
    assert check(good.replace('state=stopped', 'state=stopped ms=3 rx=1'), 'fixture') == 1
    for bad in (good.replace('state=0x201', 'state=0x20'),
                good.replace('state=stopped', 'state=stopping'),
                good.replace('state=stopped', 'state=stopped ms=3 rx=1\n'
                             'QUIESCE step=afk-timeout ep=0x22 limit_ms=2000 rx_halt=0 crashed=0 fault=00000000'),
                good.replace('after=ab', 'after=cd'),
                good.replace('status=0 quiesced', 'status=-5 quiesced'),
                good.replace('ep=0x21 word=', 'ep=0x22 word='),
                good + '\nQUIESCE step=begin\n',
                good.replace('linux_a9.entry=fixture', 'linux_a9.entry=old')):
        try:
            check(bad, 'fixture')
        except AssertionError:
            continue
        raise AssertionError('bad transcript accepted')
    replayed = '\n'.join(['AOP-PREV SHUTDOWN begin mode=quiesce entry=fixture writer=' + '11' * 16] +
                         ['AOP-PREV ' + line for line in good.splitlines()[1:]])
    assert check(replayed, 'fixture') == 1
    for bad in (replayed.replace('entry=fixture', 'entry=fixture2'),
                replayed.replace('halted=1', 'halted=0')):
        try:
            check(bad, 'fixture')
        except AssertionError:
            continue
        raise AssertionError('bad replayed transcript accepted')
    print('PASS shutdown transcript checker: complete sequence, console or reserved-RAM replay; wrong/missing ACK, failed/DMA-changing/truncated/stale entry rejected')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', nargs='?', type=Path)
    ap.add_argument('--entry')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.path or not args.entry:
        ap.error('path and --entry are required')
    paths = [args.path] if args.path.is_file() else sorted(
        p for pattern in ('records/**/*console*', '*dmesg*', '*previous_shutdown*')
        for p in args.path.glob(pattern))
    errors = []
    for path in paths:
        if not path.is_file():
            continue
        try:
            count = check(path.read_text(errors='replace'), args.entry)
        except AssertionError as error:
            errors.append(f'{path}: {error}')
            continue
        print(f'PASS retained shutdown transcript: {path}, {count} AFK ACKs; check new boot ID and both physical streams separately')
        return
    raise SystemExit('No complete matching shutdown transcript. Do not release AOP.\n' + '\n'.join(errors))


if __name__ == '__main__':
    main()
