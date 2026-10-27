#!/usr/bin/env python3
"""Check an aop_handoff=sleep warm hop, offline/read-only.

--previous: the previous kernel's shutdown transcript, from any of: the new
kernel's dmesg (AOP-PREV lines replayed from reserved RAM), debugfs
previous_shutdown, or a retained ramoops console. --current: the new
kernel's dmesg. --wrapper: the entry has aop_wake_wrapper=1. Checks logged
protocol transitions, not physical streaming.
"""
import argparse
from pathlib import Path
import re

PREVIOUS = [
    'QUIESCE step=begin',
    'QUIESCE step=rates-zero status=0',
    'QUIESCE step=ap-send word=00b0000000000010 status=0',
    'QUIESCE step=ap-ack state=0x10',
    'QUIESCE step=iop-send word=0060000000000201 status=0',
    'QUIESCE step=iop-ack state=0x201',
    'QUIESCE step=ram-stable',
    'QUIESCE step=mailbox-idle',
    'QUIESCE step=complete status=0 quiesced=1 halted=0 drained=0 handoff=1 steps=0x7ff failed_step=none',
    'HANDOFF record state=valid status=0 check=ok',
    'STAGE3 checkpoint=shutdown-handoff-complete',
]


def ordered(text, tokens):
    last = -1
    for token in tokens:
        at = text.find(token, last + 1)
        assert at > last, f'missing/out-of-order: {token}'
        last = at
    return last


def check_previous(text, entry):
    """Previous kernel: full quiesce to IOP sleep, VALID record, no halt."""
    start = text.rfind('SHUTDOWN begin mode=handoff')
    if start < 0:
        start = text.rfind('QUIESCE step=begin')
        assert start >= 0, 'no shutdown transcript'
        assert f'linux_a9.entry={entry}' in text[:start], 'previous entry not identified'
    else:
        begin = text[start:].splitlines()[0]
        assert re.search(rf'entry={re.escape(entry)}(\s|$)', begin), 'previous entry not identified'
    trace = text[start:]
    assert not re.search(r'QUIESCE (failed|step=(skipped|ap-timeout|iop-timeout|halt|drain|'
                         r'afk-(timeout|budget|unstopped)))', trace), 'failed transition or halt'
    assert 'HANDOFF failed' not in trace, 'record failed'
    ordered(trace, PREVIOUS)
    stable = next(line for line in trace.splitlines() if 'QUIESCE step=ram-stable' in line)
    assert re.search(r'window_ms=20 before=([0-9a-f]{64}) after=\1 status=0', stable), 'RAM stability failed'
    idle = next(line for line in trace.splitlines() if 'QUIESCE step=mailbox-idle' in line)
    assert idle.rstrip().endswith('status=0 halt=skipped'), 'mailbox not idle'
    sent = re.findall(r'QUIESCE step=afk-send ep=(0x[0-9a-f]+) status=0', trace)
    assert len(sent) == len(set(sent)), 'duplicate AFK shutdown'
    ap = trace.find('QUIESCE step=ap-send')
    for ep in sent:
        ack = trace.find(f'QUIESCE step=afk-ack ep={ep} state=stopped')
        disable = trace.find(f'QUIESCE step=endpoint-disable ep={ep}')
        assert 0 <= ack < disable < ap, f'AFK ACK/disable missing: {ep}'
    for line in trace.splitlines():
        if 'QUIESCE step=afk-send' in line or 'QUIESCE step=endpoint-disable' in line:
            assert line.rstrip().endswith('status=0'), line
    record = re.search(r'HANDOFF record state=valid status=0 check=ok generation=(\d+) steps=0x7ff '
                       r'entry=(\S+) writer=([0-9a-f]{32})', trace)
    assert record and record.group(2) == entry, 'record entry/writer missing'
    return dict(generation=int(record.group(1)), writer=record.group(3), afk=len(sent))


WRAPPER_STORES = [('4000', '00001111'), ('0c00', '00001111'), ('4004', '00000001'),
                  ('0c04', '00000001'), ('4004', '00001000'), ('0c04', '00001000'), ('4020', None)]


def check_wrapper(text, wrapper):
    """aop_wake_wrapper=1: the seven iOS resume stores, in order, then 0x220."""
    if not wrapper:
        assert 'HANDOFF wrapper-wake' not in text, 'wrapper wake without aop_wake_wrapper=1'
        return None
    stores = re.findall(r'COLD-W step=handoff-wrapper-wake bank=(\w+) offset=([0-9a-f]{4}) '
                        r'before=[0-9a-f]{8} value=([0-9a-f]{8})', text)
    assert len(stores) == len(WRAPPER_STORES), f'{len(stores)} wrapper stores'
    for (bank, offset, value), (want, want_value) in zip(stores, WRAPPER_STORES):
        assert bank == 'wrapper' and offset == want, f'wrapper store {offset}'
        assert want_value is None and int(value, 16) & 1 or value == want_value, f'wrapper value {offset}={value}'
    summary = re.search(r'HANDOFF wrapper-wake errno=0 writes=7 in4008=([0-9a-f]{8})->([0-9a-f]{8}) '
                        r'.* set4000=([0-9a-f]{8})->([0-9a-f]{8}) setc00=([0-9a-f]{8})->([0-9a-f]{8}) '
                        r'cpu44=([0-9a-f]{8}) run=untouched auto_boot=untouched nmi=untouched', text)
    assert summary, 'wrapper wake summary missing or failed'
    ordered(text, ['COLD checkpoint=handoff-before-wake', 'HANDOFF wrapper-wake errno=0',
                   'HANDOFF wake220 word=0060000000000220 errno=0'])
    return dict(mask=summary.group(4), mirror=summary.group(6))


def check_current(text, entry, previous=None, accel=True, wrapper=False):
    """New kernel: certified record validated, consumed once, woken, no reload."""
    assert re.search(rf'linux_a9\.entry={re.escape(entry)}(\s|$)', text), 'current entry not identified'
    assert 'HANDOFF mode=sleep record=present' in text, 'no handoff record at probe'
    for bad in ('FORCE-COLD', 'COLD-W step=cold-stop', 'COLD halt', 'HANDOFF refused',
                'failed-handoff', 'failed-rtkit-crashlog', 'AOP crashlog'):
        assert bad not in text, f'unexpected: {bad}'
    validate = re.search(r'HANDOFF record state=1 version=1 generation=(\d+) errno=0 entry=(\S+) '
                         r'writer=([0-9a-f]{32}) .*check=ok', text)
    assert validate and validate.group(2) == entry, 'record not statically valid'
    assert re.search(r'HANDOFF live errno=0 ', text), 'live check failed'
    tokens = [f'HANDOFF consumed generation={validate.group(1)}',
              'COLD checkpoint=handoff-before-wake',
              'HANDOFF wake220 word=0060000000000220 errno=0', 'HANDOFF wake first']
    last = ordered(text, tokens)
    path = re.search(r'HANDOFF wake first ep=\d+ word=[0-9a-f]+ path=(hello|resume)', text)
    assert path, 'wake reply not classified'
    ordered(text[last:], ['SYSTEM ready'] + (['STAGE3 checkpoint=iio-registered'] if accel else []))
    assert text.count('HANDOFF wake220') == 1, 'more than one wake'
    masks = check_wrapper(text, wrapper)
    # Logged from a9/aop-wake on; older kernels lack the line.
    window = re.search(r'HANDOFF wake-window end path=(\w+) in4008=([0-9a-f]{8})', text)
    assert not window or window.group(1) == path.group(1), 'wake window path mismatch'
    assert window or not wrapper, 'wake window not logged'
    if previous:
        assert int(validate.group(1)) == previous['generation'], 'generation mismatch'
        assert validate.group(3) == previous['writer'], 'record writer is not the previous kernel'
    inbox = window.group(2) if window else None
    return dict(generation=int(validate.group(1)), path=path.group(1), masks=masks,
                inbox=inbox, inbox_empty=bool(inbox and int(inbox, 16) & 0x20000))


def self_test():
    digest = 'ab' * 32
    writer = '11' * 16
    previous = '\n'.join((
        'AOP-PREV SHUTDOWN begin mode=handoff entry=fixture writer=' + writer + ' hello=1',
        'AOP-PREV QUIESCE step=begin ready=1 crashed=0 fault=00000000 pool_used=0x56000 halt=0',
        'AOP-PREV QUIESCE step=rates-zero status=0 accel=0',
        'AOP-PREV QUIESCE step=afk-send ep=0x21 status=0',
        'AOP-PREV QUIESCE step=afk-ack ep=0x21 state=stopped',
        'AOP-PREV QUIESCE step=endpoint-disable ep=0x21 word=0050002100000001 status=0',
        'AOP-PREV QUIESCE step=ap-send word=00b0000000000010 status=0',
        'AOP-PREV QUIESCE step=ap-ack state=0x10',
        'AOP-PREV QUIESCE step=iop-send word=0060000000000201 status=0',
        'AOP-PREV QUIESCE step=iop-ack state=0x201',
        f'AOP-PREV QUIESCE step=ram-stable bytes=0x56000 window_ms=20 before={digest} after={digest} status=0',
        'AOP-PREV QUIESCE step=mailbox-idle cpu44=00000000 auto_boot=00000001 in4008=00020001 out4020=00020001 status=0 halt=skipped',
        'AOP-PREV QUIESCE step=complete status=0 quiesced=1 halted=0 drained=0 handoff=1 steps=0x7ff failed_step=none errno=0',
        f'AOP-PREV HANDOFF record state=valid status=0 check=ok generation=1 steps=0x7ff entry=fixture writer={writer} pool_used=0x56000',
        'AOP-PREV STAGE3 checkpoint=shutdown-handoff-complete',
    ))
    current = '\n'.join((
        'Kernel command line: linux_a9.entry=fixture aop_handoff=sleep',
        'HANDOFF mode=sleep record=present writer=' + '22' * 16,
        f'HANDOFF record state=1 version=1 generation=1 errno=0 entry=fixture writer={writer} pool_used=0x56000 nafk=17 check=ok',
        'HANDOFF live errno=0 cpu44=00000000 auto_boot=00000001',
        'HANDOFF consumed generation=1 consumer=' + '22' * 16,
        'COLD checkpoint=handoff-before-wake defer=1',
        'HANDOFF wake220 word=0060000000000220 errno=0',
        'HANDOFF wake first ep=0 word=70000000000020 path=resume',
        'HANDOFF wake-window end path=resume in4008=00020001 out4020=00021101 wrapper=0',
        'SYSTEM ready IOP=20 AP=20 run=2 pool_used=56000 handoff=resume',
        'STAGE3 checkpoint=iio-registered',
    ))
    prev = check_previous(previous, 'fixture')
    assert check_current(current, 'fixture', prev)['path'] == 'resume'
    for bad in (previous.replace('state=0x201', 'state=0x20'),
                previous.replace('after=ab', 'after=cd'),
                previous.replace('status=0 halt=skipped', 'status=-16 halt=skipped'),
                previous + '\nAOP-PREV QUIESCE step=halt status=0',
                previous.replace('state=stopped', 'state=stopping'),
                previous.replace('entry=fixture writer', 'entry=other writer'),
                previous.replace('state=valid status=0', 'state=failed status=-110')):
        try:
            check_previous(bad, 'fixture')
        except (AssertionError, StopIteration):
            continue
        raise AssertionError('bad previous transcript accepted')
    for bad in (current + '\nFORCE-COLD stage=3', current.replace('check=ok', 'check=pool-changed'),
                current + '\nHANDOFF wake220 word=0060000000000220 errno=0',
                current.replace('path=resume', 'path=none'),
                current.replace('STAGE3 checkpoint=iio-registered', '')):
        try:
            check_current(bad, 'fixture', prev)
        except AssertionError:
            continue
        raise AssertionError('bad current transcript accepted')
    try:
        check_current(current, 'fixture', dict(prev, writer='33' * 16))
    except AssertionError:
        pass
    else:
        raise AssertionError('foreign writer accepted')
    stores = ''.join(f'COLD-W step=handoff-wrapper-wake bank=wrapper offset={o} before=00001111 '
                     f'value={v or "00021101"}\n' for o, v in WRAPPER_STORES)
    wrapped = current.replace('COLD checkpoint=handoff-before-wake defer=1', (
        'COLD checkpoint=handoff-before-wake defer=1\n' + stores +
        'HANDOFF wrapper-wake errno=0 writes=7 in4008=00021101->00021101 out4020=00021101->00021101 '
        'set4000=00001111->00000110 setc00=00000000->00000000 cpu44=00000000 run=untouched '
        'auto_boot=untouched nmi=untouched setactive=not-replayed')).replace('wrapper=0', 'wrapper=1')
    result = check_current(wrapped, 'fixture', prev, wrapper=True)
    assert result['masks'] == dict(mask='00000110', mirror='00000000') and result['inbox_empty']
    for bad in (wrapped.replace('offset=0c04 before=00001111 value=00001000', 'offset=0c04 before=00001111 value=00000000'),
                wrapped.replace('offset=4020', 'offset=0044'),
                wrapped.replace('HANDOFF wrapper-wake errno=0 writes=7', 'HANDOFF wrapper-wake errno=-5 writes=3'),
                wrapped.replace('nmi=untouched', 'nmi=written')):
        try:
            check_current(bad, 'fixture', prev, wrapper=True)
        except AssertionError:
            continue
        raise AssertionError('bad wrapper wake accepted')
    for bad, wrapper in ((current, True), (wrapped, False)):
        try:
            check_current(bad, 'fixture', prev, wrapper=wrapper)
        except AssertionError:
            continue
        raise AssertionError('wrapper mode mismatch accepted')
    print('PASS handoff checker: valid previous/current pair; wrong ACK, unstable RAM, busy mailbox, halt, AFK, entry, failed record, reload, refusal, double wake, unclassified wake, missing IIO, foreign writer rejected; wrapper wake: exact 7 stores before 0x220, wrong value/offset/failure/mode mismatch rejected')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--entry')
    ap.add_argument('--previous', type=Path, action='append', default=[],
                    help='transcript source; directories are searched for console/dmesg/previous_shutdown files')
    ap.add_argument('--current', type=Path, help="new kernel's dmesg")
    ap.add_argument('--sys', action='store_true', help='conservative entry: no IIO expected')
    ap.add_argument('--wrapper', action='store_true', help='aop_wake_wrapper=1 entry: require the resume wrapper stores')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.entry or not args.previous:
        ap.error('--entry and --previous are required')
    paths = []
    for path in args.previous:
        paths += [path] if path.is_file() else sorted(
            p for pattern in ('records/**/*console*', '*dmesg*', '*previous_shutdown*')
            for p in path.glob(pattern))
    errors, previous = [], None
    for path in paths:
        try:
            previous = check_previous(path.read_text(errors='replace'), args.entry)
        except (AssertionError, StopIteration) as error:
            errors.append(f'{path}: {error}')
            continue
        print(f'PASS previous-kernel handoff transcript: {path}, generation {previous["generation"]}, '
              f'writer {previous["writer"]}, {previous["afk"]} AFK ACKs')
        break
    if not previous:
        raise SystemExit('No complete matching handoff transcript. Do not trust this hop.\n' + '\n'.join(errors))
    if args.current:
        current = check_current(args.current.read_text(errors='replace'), args.entry, previous,
                                not args.sys, args.wrapper)
        print(f'PASS current kernel: record gen {current["generation"]} validated, consumed once, '
              f'woken via {current["path"]} path, no halt/reload; check streaming/tilt separately')
        print(f'wake window: in4008={current["inbox"]} (empty={int(current["inbox_empty"])}), '
              f'wrapper masks={current["masks"]}')


if __name__ == '__main__':
    main()
