#!/usr/bin/env python3
"""Check an aop_spi0_reset=1 reload of a live AOP, offline/read-only.

Input: the reloading kernel's dmesg (capture-stage23.sh dmesg.txt), after
trigger-cold.sh. Requires: halt and drain, a retained-state decision to run
(a9/aop-wake2: `SPI0 reset decision=run reason=...`, optionally a given
reason), the SPI0 reset with +0x8d0 bit 6 clear, the pre-RUN recheck still
clear, release, then the full stage-3 continuation through IIO with no
crashlog. Checks logged transitions, not physical streaming.

--ch3td (a9/aop-wake3) also requires the channel-3 teardown after the reset
(ds-aop-spi0-ch3-round3.md section 5): its flushed begin line, the exact
readback bits, and the >= 150 ms pre-RUN window of 31 clear samples.

--intc (a9/aop-wake6, ds-aop-spi0-ch3-round4.md section 3) also requires AOP
IRQ 0x10014 disabled (+0xa00 = 00100000) after the decision and before the
SPI0 reset, and acknowledged (+0x980 = 00100000) after the clean window and
before release, with no SCMErr. Readbacks and the logged SCM/interval/+0x81c
words are reported, never compared.

--wake7 (a9/aop-wake7) is --ch3td --intc plus: the channel-3 idle wait's
per-slice samples (slice 0..n-1, only the last idle) and its summary (idle
within the 20 ms bound; beyond_fw_bound reported), and the four INTC state
snapshots around both stores, in order, with 0x10014 masked (src_masked=1)
after the disable. view=all-ones and the +0x81c/+0x980 words are reported.
"""
import argparse
from pathlib import Path
import re

RUN_REASONS = ('live-auto-boot', 'prev-quiesce-halted', 'prev-live', 'prev-carried')
CH3_REGS = r'c3=([0-9a-f]{8}) route=([0-9a-f]{8}) rate=([0-9a-f]{8}) c3c=([0-9a-f]{8}) desc=([0-9a-f]{8}) 8d0=([0-9a-f]{8}) 8d4=([0-9a-f]{8})'


def ordered(text, tokens):
    last = -1
    for token in tokens:
        at = text.find(token, last + 1)
        assert at > last, f'missing/out-of-order: {token}'
        last = at
    return last


IDLE_SAMPLE = (r'SPI0 CH3TD idle-sample slice=(\d+) at_us=(\d+) c3=([0-9a-f]{8}) scm_lo=([0-9a-f]{8}) '
               r'scm_hi=([0-9a-f]{8}) 8d0=([0-9a-f]{8}) idle=([01])')
IDLE_POLL = (r'SPI0 CH3TD idle-poll run_before=([01]) slices=(\d+)/20 idle_us=(\d+) bound_us=20000 '
             r'fw_bound_us=10000 beyond_fw_bound=([01]) result=(idle|timeout)')


def check_idle(text):
    """Wake7: the sliced idle wait between the stop store and the masks."""
    samples = re.findall(IDLE_SAMPLE, text)
    poll = re.findall(IDLE_POLL, text)
    assert len(poll) == 1, 'exactly one CH3TD idle-poll line'
    run_before, slices, idle_us, beyond, result = poll[0]
    slices, idle_us = int(slices), int(idle_us)
    assert result == 'idle', 'channel 3 never went idle'
    assert 1 <= slices <= 20 and len(samples) == slices, 'idle-sample count disagrees with slices'
    assert [int(row[0]) for row in samples] == list(range(slices)), 'idle samples not 0..n-1'
    assert [row[6] for row in samples] == ['0'] * (slices - 1) + ['1'], 'only the last sample may be idle'
    assert all(int(row[2], 16) & 0x2 == int(row[6]) * 2 for row in samples), 'idle flag disagrees with C3'
    assert idle_us <= 20000 and int(beyond) == (idle_us > 10000), 'idle time / firmware-bound flag'
    ordered(text, ['SPI0 CH3TD before ', 'SPI0 CH3TD idle-sample slice=0 ', 'SPI0 CH3TD idle-poll ',
                   'SPI0 CH3TD writes '])
    return dict(run_before=int(run_before), slices=slices, idle_us=idle_us, beyond_fw_bound=int(beyond),
                samples=[(int(row[1]), int(row[2], 16), int(row[3], 16), int(row[4], 16), int(row[5], 16))
                         for row in samples])


def check_ch3td(text):
    """Channel-3 teardown lines: every readback bit, then the pre-RUN window."""
    for bad in ('failed-ch3', 'CH3TD reasserted', 'masks=skipped'):
        assert bad not in text, f'unexpected: {bad}'
    before = re.findall(r'SPI0 CH3TD before ' + CH3_REGS, text)
    writes = re.findall(r'SPI0 CH3TD writes stop=([0-9a-f]{8}) idle_poll=([0-9a-f]{8}) poll_errno=0 '
                        r'class0=([0-9a-f]{8}) class1=([0-9a-f]{8}) desc=00000000 c3c=([0-9a-f]{8})', text)
    after = re.findall(r'SPI0 CH3TD after ' + CH3_REGS, text)
    check = re.findall(r'SPI0 CH3TD check errno=0 run=0 class0=0 class1=0 idle=1 desc=00000000 '
                       r'c3c_bit5=([01]) ch3_pending=0 result=pass', text)
    window = re.findall(r'SPI0 CH3TD window errno=0 samples=31/31 elapsed_ms=(\d+) min_ms=150 result=pass', text)
    assert len(before) == len(writes) == len(after) == len(check) == len(window) == 1, \
        'exactly one CH3TD before/writes/after/check(pass)/window'
    before = [int(v, 16) for v in before[0]]
    stop, idle, class0, class1, ack = (int(v, 16) for v in writes[0])
    after = [int(v, 16) for v in after[0]]
    c3, c3c, desc, fault = after[0], after[3], after[4], after[5]
    # Section 5 only: the cleared/set bits. Bits 10/16 of C3 and 0/21 of C3C
    # are unnamed, so whole words are reported, not pinned. C3C bit 5 is a
    # self-clearing pulse (19:50): it must be written, its readback is only
    # recorded, and the acknowledge is judged by +0x8d0 bit 6.
    assert not stop & 0x411 and not class0 & 0x450 and not class1 & 0x510 and ack & 0x20, 'written words'
    assert idle & 0x2, 'idle bit 1 never set'
    assert not c3 & 0x141, 'C3 run/class-0/class-1 bit still set'
    assert c3 & 0x2 and desc == 0 and not fault & 0x40, 'teardown readback'
    assert int(check[0]) == bool(c3c & 0x20), 'c3c_bit5 disagrees with the C3C readback'
    elapsed = int(window[0])
    assert elapsed >= 150, 'window shorter than 150 ms'
    ordered(text, ['SPI0 reset errno=0', 'SPI0 CH3TD begin pa=210100280 cpu=halted',
                   'COLD checkpoint=spi0-ch3-teardown', 'SPI0 CH3TD before ', 'SPI0 CH3TD writes ',
                   'SPI0 CH3TD after ', 'SPI0 CH3TD check errno=0 ', 'SPI0 CH3TD window errno=0 samples=31/31',
                   'SPI0 pre-run errno=0', 'AOP released'])
    return dict(c3=(before[0], stop, class0, c3), c3c=(before[3], ack, c3c), desc=(before[4], desc),
                fault=(before[5], fault), elapsed_ms=elapsed)


HEX = r'([0-9a-f]{8})'
SCM_WORDS = rf'scm_lo={HEX} scm_hi={HEX} c3_interval={HEX} c3_cursor={HEX} current={HEX}'
STATE_AT = ('pre-disable', 'post-disable', 'pre-ack', 'post-ack')
INTC_STATE = (rf'AOP INTC state at=([a-z-]+) reset808={HEX} current81c={HEX} raise900={HEX} ack980={HEX} '
              rf'mask_a00={HEX} mask_a04={HEX} mask_a08={HEX} mask_a80={HEX} g7_c020={HEX} g7_c024={HEX} '
              r'src_masked=([01]) view=(intc|all-ones)')


def check_states(text):
    """Wake7: the ten INTC words around both stores; 0x10014 masked after the disable."""
    rows = re.findall(INTC_STATE, text)
    assert [row[0] for row in rows] == list(STATE_AT), 'INTC state lines missing, extra or out of order'
    words = {row[0]: tuple(int(v, 16) for v in row[1:11]) for row in rows}
    for at, row in zip(STATE_AT, rows):
        assert int(row[11]) == bool(words[at][4] & 0x00100000), f'src_masked disagrees with mask_a00 at {at}'
        assert (row[12] == 'all-ones') == (words[at][1] == 0xffffffff), f'view disagrees with +0x81c at {at}'
    assert rows[1][11] == '1', '0x10014 not masked after the disable'
    ordered(text, ['AOP INTC disable begin', 'AOP INTC state at=pre-disable', 'COLD-W step=intc-disable',
                   'AOP INTC state at=post-disable', 'AOP INTC disable errno=0', 'SPI0 reset begin pa=210100000',
                   'AOP INTC state at=pre-ack', 'AOP INTC pre-ack', 'COLD-W step=intc-ack',
                   'AOP INTC state at=post-ack', 'AOP INTC ack errno=0', 'AOP INTC post-ack errno=0'])
    return dict(words=words, views={row[0]: row[12] for row in rows})


def check_intc(text, states=False):
    """Round-4 INTC: disable before the reset, acknowledge after the window."""
    for bad in ('failed-intc', 'SCMErr'):
        assert bad not in text, f'unexpected: {bad}'
    # Wake7 adds the mask-readback fields (src_masked=1 is the kernel's gate).
    extra = r' mask_a80=[0-9a-f]{8} src_masked=1 view=(?:intc|all-ones)' if states else ''
    lines = dict(
        begin=re.findall(r'AOP INTC disable begin irq=0x10014 pa=210800a00 value=00100000 cpu=halted', text),
        disable=re.findall(rf'AOP INTC disable errno=0 before={HEX} value=00100000 after={HEX}{extra} result=pass',
                           text),
        pre=re.findall(rf'AOP INTC pre-ack {SCM_WORDS} cpu=halted', text),
        ack=re.findall(rf'AOP INTC ack errno=0 before={HEX} value=00100000 after={HEX}', text),
        post=re.findall(rf'AOP INTC post-ack errno=0 {SCM_WORDS} result=pass', text),
        stores=re.findall(r'COLD-W step=intc-(disable|ack) bank=wrapper offset=(0a00|0980) '
                          r'before=[0-9a-f]{8} value=00100000', text))
    counts = {k: len(v) for k, v in lines.items()}
    assert counts == dict(begin=1, disable=1, pre=1, ack=1, post=1, stores=2), counts
    assert text.count('AOP INTC ') == 5 + 4 * states, 'extra or malformed AOP INTC line'
    assert lines['stores'] == [('disable', '0a00'), ('ack', '0980')], lines['stores']
    ordered(text, ['SPI0 reset decision=run', 'AOP INTC disable begin', 'COLD-W step=intc-disable',
                   'AOP INTC disable errno=0', 'SPI0 reset begin pa=210100000', 'SPI0 pre-run errno=0',
                   'AOP INTC pre-ack', 'COLD-W step=intc-ack', 'AOP INTC ack errno=0',
                   'AOP INTC post-ack errno=0', 'AOP released'])
    words = lambda row: tuple(int(v, 16) for v in row)
    return dict(disable=words(lines['disable'][0]), ack=words(lines['ack'][0]),
                pre=words(lines['pre'][0]), post=words(lines['post'][0]),
                states=check_states(text) if states else None)


SCM_STAGES = ('before-controller-reset', 'after-controller-reset', 'after-ch3-teardown',
              'after-copy', 'after-window', 'pre-intc-reset', 'post-intc-reset')


def check_reset(text):
    """Wake8's event-3 pulse and seven read-only milestones; SCM need not be 0."""
    assert 'failed-intc' not in text and 'failed-spi0-trace' not in text
    rows = re.findall(r'SPI0 SCM stage=([a-z0-9-]+) errno=0 ctl=' + HEX + r' clk=' + HEX +
                      r' scm_lo=' + HEX + r' scm_hi=' + HEX + r' c3_interval=' + HEX +
                      r' c3_cursor=' + HEX + ' ' + CH3_REGS + r' cpu=halted', text)
    assert [row[0] for row in rows] == list(SCM_STAGES), 'missing/extra/misordered SCM milestones'
    assert text.count('SPI0 SCM ') == len(SCM_STAGES), 'malformed SCM milestone'
    final = tuple(int(v, 16) for v in rows[-1][1:])
    # ctl clk scm_lo scm_hi interval cursor c3 route rate c3c desc 8d0 8d4
    assert not final[6] & 0x141 and final[6] & 2 and not final[10] and not final[11] & 0x40
    stores = re.findall(r'COLD-W step=([a-z-]+) bank=wrapper offset=0808 before=' + HEX +
                        r' value=' + HEX, text)
    assert len(stores) == 2 and [(r[0], int(r[2], 16)) for r in stores] == [('intc-reset', 1), ('intc-reset', 0)]
    reset = re.findall(r'AOP INTC reset errno=0 asserted=' + HEX + r' after=00000000 result=pass', text)
    assert len(reset) == 1 and text.count('AOP INTC reset ') == 2
    assert text.count('AOP INTC reset begin pa=210800808 sequence=1/read/0/read cpu=halted') == 1
    tail = text[text.index('AOP INTC reset begin'):text.index('AOP released')]
    reads = re.findall(r'COLD-R offset=0808 after=' + HEX, tail)
    assert len(reads) == 2 and int(reads[1], 16) == 0, 'missing ordering reads or reset still asserted'
    ordered(text, ['COLD checkpoint=spi0-reset', 'SPI0 SCM stage=before-controller-reset',
                   'SPI0 reset errno=0', 'SPI0 SCM stage=after-controller-reset', 'SPI0 CH3TD begin',
                   'SPI0 CH3TD check errno=0', 'SPI0 SCM stage=after-ch3-teardown',
                   'SPI0 SCM stage=after-copy', 'SPI0 CH3TD window errno=0',
                   'SPI0 SCM stage=after-window', 'AOP INTC post-ack errno=0',
                   'SPI0 SCM stage=pre-intc-reset', 'AOP INTC reset begin',
                   'COLD-W step=intc-reset bank=wrapper offset=0808', 'COLD-R offset=0808',
                   'COLD-W step=intc-reset bank=wrapper offset=0808', 'COLD-R offset=0808',
                   'SPI0 SCM stage=post-intc-reset', 'AOP INTC reset errno=0', 'AOP released'])
    cpu = re.findall(r'COLD-W .*offset=0044 .*value=' + HEX, text)
    assert len(cpu) <= 2 and sum(bool(int(v, 16) & 0x10) for v in cpu) == 1, 'RUN must be written exactly once'
    return dict(asserted=reset[0], scm_final=final[2:4])


def check(text, entry, accel=True, reason=None, ch3td=False, intc=False, wake7=False, wake8=False):
    # The previous (failed handoff) kernel's replayed transcript is not this boot.
    text = '\n'.join(line for line in text.splitlines() if 'AOP-PREV' not in line)
    wake8_reset = check_reset(text) if wake8 else None
    if wake8:
        wake7 = True
        # The previous checker deliberately counts its nine INTC lines.
        text = '\n'.join(line for line in text.splitlines() if 'AOP INTC reset ' not in line)
    assert re.search(rf'linux_a9\.entry={re.escape(entry)}(\s|$)', text), 'entry not identified'
    assert re.search(r'EXPERIMENT spi0_reset=1 wake_wrapper=0 .*spi0_applies=live-reload', text), \
        'aop_spi0_reset=1 not armed'
    for bad in ('AOP crashlog', 'failed-rtkit-crashlog', 'failed-spi0', 'failed-startep',
                'failed-cold-load', 'SPI0 reset skipped', 'SPI0 reset decision=skip', 'HANDOFF'):
        assert bad not in text, f'unexpected: {bad}'
    decision = re.findall(r'SPI0 reset decision=run reason=([a-z-]+) auto_boot=(\d+) out4020=([0-9a-f]{8}) '
                          r'prev=(missing|invalid|valid) prev_hello=(\d) prev_halted=(\d) '
                          r'prev_steps=(0x[0-9a-f]+) prev_carry=(\d)', text)
    assert len(decision) == 1, 'exactly one SPI0 decision=run line required'
    decision = decision[0]
    assert decision[0] in RUN_REASONS, f'not a run reason: {decision[0]}'
    if reason:
        assert decision[0] == reason, f'decision reason {decision[0]}, expected {reason}'
    reset = re.search(r'SPI0 reset errno=0 ran=1 pa=210100000 before ctl=([0-9a-f]{8}) clk=[0-9a-f]{8} '
                      r'8d0=([0-9a-f]{8})', text)
    after = re.search(r'SPI0 reset after ctl=([0-9a-f]{8}) .* 8d0=([0-9a-f]{8}) .*saved=([0-9a-f]{8}) '
                      r'asserted=([0-9a-f]{8}) polled=([0-9a-f]{8}) ch3_pending=(\d)->0 provider3=not-replayed', text)
    prerun = re.search(r'SPI0 pre-run errno=0 8d0=([0-9a-f]{8}) ch3_pending=0 cpu=halted', text)
    pmgr = re.search(r'SPI0 pmgr ps_spi0 before=([0-9a-f]{8}) errno=(-?\d+) after=([0-9a-f]{8}) errno=(-?\d+)', text)
    assert reset and after and prerun and pmgr, 'SPI0 reset/pre-run evidence missing'
    assert int(after.group(2), 16) & 0x40 == 0 and int(prerun.group(1), 16) & 0x40 == 0
    assert int(after.group(4), 16) == (int(after.group(3), 16) & ~0x101) | 0x100, 'reset word'
    ordered(text, ['FORCE-COLD stage=3', 'COLD halt errno=0', 'SPI0 reset decision=run',
                   'SPI0 reset begin pa=210100000 cpu=halted',
                   'COLD checkpoint=spi0-reset', 'SPI0 reset errno=0', 'SPI0 pre-run errno=0',
                   'AOP released'])
    tail = ['STAGE3 checkpoint=system-ready', 'STAGE3 checkpoint=application-endpoints-enabled']
    if accel:
        tail += ['STAGE3 checkpoint=accel-service-ready', 'STAGE3 checkpoint=iio-registered']
    ordered(text[text.find('AOP released'):], tail)
    ch3td, intc = ch3td or wake7, intc or wake7
    teardown = check_ch3td(text) if ch3td else None
    if wake7:
        teardown['idle'] = check_idle(text)
    return dict(reset=wake8_reset, intc=check_intc(text, wake7) if intc else None, teardown=teardown, reason=decision[0], auto_boot=int(decision[1]), out4020=decision[2], prev=decision[3],
                prev_steps=decision[6], before_ctl=reset.group(1), before_8d0=reset.group(2), after_8d0=after.group(2),
                pending_before=int(after.group(6)), prerun=prerun.group(1),
                pmgr=(pmgr.group(1), int(pmgr.group(2)), pmgr.group(3), int(pmgr.group(4))))


def self_test():
    decision = ('SPI0 reset decision=run reason=prev-quiesce-halted auto_boot=0 out4020=00021100 prev=valid '
                'prev_hello=1 prev_halted=1 prev_steps=0x1bff prev_carry=1')
    text = '\n'.join((
        'Kernel command line: linux_a9.entry=fixture aop_quiesce=1 aop_spi0_reset=1',
        'EXPERIMENT spi0_reset=1 wake_wrapper=0 setactive=not-implemented automatic_fallback=none '
        'spi0_applies=live-reload spi0_pmgr=ps_spi0',
        'FORCE-COLD stage=3 attempt=1; owner accepts crash risk; stop before load, no reset hook',
        'COLD halt errno=0 predicate=run-clear-delay cpu44=00000000',
        decision,
        'SPI0 reset begin pa=210100000 cpu=halted ps_spi0=00000200 errno=-112: first AP access follows',
        'COLD checkpoint=spi0-reset defer=1 test=5 stage=3 ap_irqs=masked',
        'SPI0 reset errno=0 ran=1 pa=210100000 before ctl=00000011 clk=01b71b00 8d0=00000040 8d4=00000000 ch3=00000000 ch3c=00000000',
        'SPI0 reset after ctl=00000011 clk=01b71b00 8d0=00000000 8d4=00000000 ch3=00000000 ch3c=00000000 '
        'saved=00000011 asserted=00000110 polled=00000110 ch3_pending=1->0 provider3=not-replayed',
        'SPI0 pmgr ps_spi0 before=00000200 errno=-112 after=00000200 errno=-112 (read-only, never powered)',
        'SPI0 pre-run errno=0 8d0=00000000 ch3_pending=0 cpu=halted',
        'AOP released: image 342336 B',
        'STAGE3 checkpoint=system-ready',
        'STAGE3 checkpoint=application-endpoints-enabled',
        'STAGE3 checkpoint=accel-service-ready',
        'STAGE3 checkpoint=iio-registered',
    ))
    result = check(text, 'fixture')
    assert check(text, 'fixture', reason='prev-quiesce-halted') == result
    assert result['reason'] == 'prev-quiesce-halted' and result['out4020'] == '00021100'
    replayed = text.replace('FORCE-COLD stage=3', 'AOP-PREV HANDOFF record state=failed status=-107\n'
                            'AOP-PREV QUIESCE step=skipped result=failed-handoff-no-wake-reply\nFORCE-COLD stage=3')
    assert check(replayed, 'fixture') == result
    # The previous kernel's replayed carry/decision lines are not this boot's.
    replayed = text.replace('FORCE-COLD stage=3', 'AOP-PREV SPI0-CARRY retained=1 source=hello\n'
                            'AOP-PREV SPI0 reset decision=skip reason=fresh-instance\nFORCE-COLD stage=3')
    assert check(replayed, 'fixture') == result
    moved = '\n'.join(line for line in text.splitlines() if line != decision)
    assert result['pending_before'] == 1 and result['pmgr'] == ('00000200', -112, '00000200', -112)
    for bad in (text.replace('ch3_pending=1->0', 'ch3_pending=1->1'),
                text.replace('SPI0 pre-run errno=0 8d0=00000000 ch3_pending=0',
                             'SPI0 pre-run errno=-5 8d0=00000040 ch3_pending=1'),
                text.replace('asserted=00000110', 'asserted=00000111'),
                text + '\nAOP crashlog section=CrgA',
                text.replace('STAGE3 checkpoint=iio-registered', 'failed-startep'),
                text.replace('spi0_reset=1 wake_wrapper=0', 'spi0_reset=0 wake_wrapper=0'),
                text.replace('SPI0 reset errno=0 ran=1', 'SPI0 reset skipped reason=fresh-instance\nSPI0 reset errno=0 ran=1'),
                text.replace('decision=run reason=prev-quiesce-halted', 'decision=skip reason=fresh-instance'),
                text.replace('decision=run reason=prev-quiesce-halted', 'decision=run reason=stale-transcript'),
                text.replace('SPI0 reset decision=run', 'SPI0 reset decided=run'),
                text.replace('SPI0 reset begin pa', decision.replace('prev-quiesce-halted', 'live-auto-boot') +
                             '\nSPI0 reset begin pa'),
                # The decision must follow the halt.
                moved.replace('COLD halt errno=0', decision + '\nCOLD halt errno=0')):
        try:
            check(bad, 'fixture')
        except AssertionError:
            continue
        raise AssertionError('bad SPI0 reload accepted')
    try:
        check(text, 'fixture', reason='live-auto-boot')
    except AssertionError:
        pass
    else:
        raise AssertionError('wrong decision reason accepted')
    print('PASS SPI0 reload checker: one decision=run line after the halt, reset word, bit 6 clear after reset '
          'and before RUN, order, IIO; sticky/reasserted bit, wrong word, crashlog, STARTEP failure, unarmed, '
          'skipped, decision=skip, non-run/missing/duplicate/misplaced decision and wrong --reason rejected')
    # --ch3td: the 19:50 device words (a9/aop-wake4 refused them as
    # failed-ch3-teardown-ack; C3C bit 5 is a pulse), as a9/aop-wake5 logs them.
    teardown = '\n'.join((
        'SPI0 CH3TD begin pa=210100280 cpu=halted: first channel-3 store follows',
        'COLD checkpoint=spi0-ch3-teardown defer=1 test=5 stage=3 ap_irqs=masked',
        'SPI0 CH3TD before c3=01011445 route=00000010 rate=019000c8 c3c=00200009 desc=00000000 8d0=00000040 8d4=00000000',
        'SPI0 CH3TD writes stop=01011044 idle_poll=02010446 poll_errno=0 class0=02010006 class1=02010006 '
        'desc=00000000 c3c=00200029',
        'SPI0 CH3TD after c3=00010406 route=00000010 rate=019000c8 c3c=00200009 desc=00000000 8d0=00000000 8d4=00000000',
        'SPI0 CH3TD check errno=0 run=0 class0=0 class1=0 idle=1 desc=00000000 c3c_bit5=0 ch3_pending=0 result=pass',
        'SPI0 CH3TD window errno=0 samples=31/31 elapsed_ms=151 min_ms=150 result=pass'))
    full = text.replace('SPI0 pre-run errno=0', teardown + '\nSPI0 pre-run errno=0')
    torn = check(full, 'fixture', reason='prev-quiesce-halted', ch3td=True)['teardown']
    assert torn == dict(c3=(0x1011445, 0x1011044, 0x2010006, 0x10406), c3c=(0x200009, 0x200029, 0x200009),
                        desc=(0, 0), fault=(0x40, 0), elapsed_ms=151), torn
    assert check(full, 'fixture')['teardown'] is None
    # A C3C bit 5 that does read back set is recorded, not required either way.
    latched = full.replace('after c3=00010406 route=00000010 rate=019000c8 c3c=00200009',
                           'after c3=00010406 route=00000010 rate=019000c8 c3c=00200029').replace(
                           'c3c_bit5=0', 'c3c_bit5=1')
    assert check(latched, 'fixture', ch3td=True)['teardown']['c3c'][2] == 0x200029
    window = teardown.splitlines()[-1]
    for bad in (text,  # a9/aop-wake2: no teardown at all
                full.replace('result=pass\nSPI0 CH3TD window', 'result=failed-ch3-teardown-pending\nSPI0 CH3TD window'),
                # a9/aop-wake4's check line: the readback gate that 19:50 disproved.
                full.replace('c3c_bit5=0 ch3_pending=0 result=pass', 'ack=0 ch3_pending=0 result=failed-ch3-teardown-ack'),
                full.replace('c3c_bit5=0', 'c3c_bit5=1'),
                full.replace('desc=00000000 c3c=00200029', 'desc=00000000 c3c=00200009'),
                full.replace(window, 'SPI0 CH3TD reasserted sample=20 at_ms=100 c3=00010006\n'
                             'SPI0 CH3TD window errno=-5 samples=21/31 elapsed_ms=100 min_ms=150 '
                             'result=failed-ch3-reasserted'),
                full.replace('elapsed_ms=151', 'elapsed_ms=149'),
                full.replace('samples=31/31', 'samples=30/31'),
                full.replace('COLD checkpoint=spi0-ch3-teardown', 'COLD checkpoint=spi0-ch3'),
                full.replace('after c3=00010406', 'after c3=00010446'),
                full.replace('after c3=00010406', 'after c3=00010407'),
                full.replace('after c3=00010406', 'after c3=00010506'),
                full.replace('after c3=00010406', 'after c3=00010404'),
                full.replace('c3c=00200009 desc=00000000 8d0=00000000', 'c3c=00200009 desc=00000400 8d0=00000000'),
                full.replace('desc=00000000 8d0=00000000 8d4=00000000\nSPI0 CH3TD check',
                             'desc=00000000 8d0=00000040 8d4=00000000\nSPI0 CH3TD check'),
                full.replace('idle_poll=02010446 poll_errno=0 class0=02010006 class1=02010006 desc=00000000 '
                             'c3c=00200029', 'idle_poll=01011044 poll_errno=-110 masks=skipped'),
                full.replace(teardown, '\n'.join(teardown.splitlines()[::-1])),
                full.replace(window + '\n', '') + '\n' + window):
        try:
            check(bad, 'fixture', ch3td=True)
        except AssertionError:
            continue
        raise AssertionError('bad channel-3 teardown accepted')
    print('PASS SPI0 reload checker --ch3td: 19:50 words pass (C3C bit 5 pulse, readback recorded either way); '
          'flushed teardown begin after the reset, run/class0/class1 clear, idle set, desc 0, C3C bit 5 written, '
          '8d0 bit 6 clear, 31/31 window >= 150 ms before RUN; missing teardown, failed-ch3-*, the wake4 ack gate, '
          'unwritten ack, mismatched c3c_bit5, reassert, short/partial window, any bad readback, skipped masks '
          'and wrong order rejected')
    # --intc: the a9/aop-wake6 lines around the same teardown.
    disable = '\n'.join((
        'AOP INTC disable begin irq=0x10014 pa=210800a00 value=00100000 cpu=halted: first INTC store follows',
        'COLD checkpoint=intc-disable defer=1 test=5 stage=3 ap_irqs=masked',
        'COLD-W step=intc-disable bank=wrapper offset=0a00 before=00000000 value=00100000',
        'COLD-R offset=0a00 after=00000000',
        'AOP INTC disable errno=0 before=00000000 value=00100000 after=00000000 result=pass'))
    ack = '\n'.join((
        'AOP INTC pre-ack scm_lo=00000003 scm_hi=00000000 c3_interval=00000000 c3_cursor=00000000 '
        'current=00010014 cpu=halted',
        'COLD-W step=intc-ack bank=wrapper offset=0980 before=00000000 value=00100000',
        'COLD-R offset=0980 after=00000000',
        'AOP INTC ack errno=0 before=00000000 value=00100000 after=00000000',
        'AOP INTC post-ack errno=0 scm_lo=00000003 scm_hi=00000000 c3_interval=00000000 c3_cursor=00000000 '
        'current=00000000 result=pass'))
    both = full.replace('SPI0 reset begin pa', disable + '\nSPI0 reset begin pa').replace(
        'AOP released', ack + '\nAOP released')
    got = check(both, 'fixture', reason='prev-quiesce-halted', ch3td=True, intc=True)
    assert got['intc'] == dict(disable=(0, 0), ack=(0, 0), pre=(3, 0, 0, 0, 0x10014), post=(3, 0, 0, 0, 0),
                               states=None), got
    assert check(both, 'fixture', ch3td=True)['intc'] is None
    # Undocumented readbacks are reported whatever they read.
    assert check(both.replace('after=00000000\nAOP INTC ack', 'after=00100000\nAOP INTC ack').replace(
        'value=00100000 after=00000000\nAOP INTC post', 'value=00100000 after=00100000\nAOP INTC post'),
        'fixture', ch3td=True, intc=True)['intc']['ack'] == (0, 0x100000)
    for bad in (full,  # a9/aop-wake5: no INTC lines
                both.replace(disable + '\n', ''),
                both.replace(ack + '\n', ''),
                both.replace('result=pass\nSPI0 reset begin', 'result=failed-intc-disable\nSPI0 reset begin'),
                both.replace('AOP INTC ack errno=0', 'AOP INTC ack errno=-5'),
                both.replace('post-ack errno=0', 'post-ack errno=-112'),
                both.replace('offset=0980 before=00000000 value=00100000', 'offset=0980 before=00000000 value=00000001'),
                both.replace('offset=0a00 before=00000000 value=00100000', 'offset=0a04 before=00000000 value=00100000'),
                both + '\nAOP crashlog section=Cstr SCMErr:0x3,3',
                # disable after the first SPI0 access / acknowledge before the window
                both.replace(disable + '\n', '').replace('SPI0 pmgr ps_spi0', disable + '\nSPI0 pmgr ps_spi0'),
                both.replace(ack + '\n', '').replace('SPI0 CH3TD begin', ack + '\nSPI0 CH3TD begin'),
                both.replace(ack + '\n', '').replace('STAGE3 checkpoint=system-ready',
                                                     ack + '\nSTAGE3 checkpoint=system-ready'),
                both.replace('AOP INTC pre-ack', 'AOP INTC pre-ack x\nAOP INTC pre-ack', 1)):
        try:
            check(bad, 'fixture', ch3td=True, intc=True)
        except AssertionError:
            continue
        raise AssertionError('bad INTC sequence accepted')
    print('PASS SPI0 reload checker --intc: 0x10014 disabled at +0xa00 after the decision and before the SPI0 reset, '
          'acknowledged at +0x980 after the window and before release, readbacks reported not gated; missing, '
          'failed, misordered, wrong-offset/value, duplicated INTC lines and SCMErr rejected')
    # --wake7: the same run as a9/aop-wake7 logs it, with the post-quiesce
    # INTC words (every bank-0 source masked) and idle after two busy slices.
    state = lambda at, cur='00000000', mask='ffffffff': (
        f'AOP INTC state at={at} reset808=00000000 current81c={cur} raise900=00000000 ack980=00000000 '
        f'mask_a00={mask} mask_a04=ffffffff mask_a08=ffffffff mask_a80={mask} g7_c020=00000002 g7_c024=00000002 '
        f'src_masked={int(bool(int(mask, 16) & 0x100000))} view={"all-ones" if cur == "ffffffff" else "intc"}')
    disable7 = '\n'.join((
        'AOP INTC disable begin irq=0x10014 pa=210800a00 value=00100000 cpu=halted: first INTC store follows',
        'COLD checkpoint=intc-disable defer=1 test=5 stage=3 ap_irqs=masked',
        state('pre-disable'),
        'COLD-W step=intc-disable bank=wrapper offset=0a00 before=ffffffff value=00100000',
        'COLD-R offset=0a00 after=ffffffff',
        state('post-disable'),
        'AOP INTC disable errno=0 before=ffffffff value=00100000 after=ffffffff mask_a80=ffffffff src_masked=1 '
        'view=intc result=pass'))
    ack7 = '\n'.join((
        state('pre-ack'),
        'AOP INTC pre-ack scm_lo=00000003 scm_hi=00000000 c3_interval=00000000 c3_cursor=00000000 '
        'current=00000000 cpu=halted',
        'COLD-W step=intc-ack bank=wrapper offset=0980 before=00000000 value=00100000',
        'COLD-R offset=0980 after=00000000',
        state('post-ack'),
        'AOP INTC ack errno=0 before=00000000 value=00100000 after=00000000',
        'AOP INTC post-ack errno=0 scm_lo=00000003 scm_hi=00000000 c3_interval=00000000 c3_cursor=00000000 '
        'current=00000000 result=pass'))
    idle7 = '\n'.join((
        'SPI0 CH3TD idle-sample slice=0 at_us=1001 c3=02010444 scm_lo=00000003 scm_hi=00000000 8d0=00000040 idle=0',
        'SPI0 CH3TD idle-sample slice=1 at_us=2002 c3=02010444 scm_lo=00000003 scm_hi=00000000 8d0=00000040 idle=0',
        'SPI0 CH3TD idle-sample slice=2 at_us=2310 c3=02010446 scm_lo=00000003 scm_hi=00000000 8d0=00000040 idle=1',
        'SPI0 CH3TD idle-poll run_before=1 slices=3/20 idle_us=2310 bound_us=20000 fw_bound_us=10000 '
        'beyond_fw_bound=0 result=idle'))
    w7 = full.replace('SPI0 CH3TD writes ', idle7 + '\nSPI0 CH3TD writes ').replace(
        'SPI0 reset begin pa', disable7 + '\nSPI0 reset begin pa').replace('AOP released', ack7 + '\nAOP released')
    got = check(w7, 'fixture', reason='prev-quiesce-halted', wake7=True)
    idle = got['teardown']['idle']
    assert (idle['run_before'], idle['slices'], idle['idle_us'], idle['beyond_fw_bound']) == (1, 3, 2310, 0), idle
    assert idle['samples'][0] == (1001, 0x02010444, 3, 0, 0x40), idle
    states = got['intc']['states']
    assert states['views'] == dict.fromkeys(STATE_AT, 'intc') and states['words']['post-disable'][4] == 0xffffffff
    assert got['intc']['disable'] == (0xffffffff, 0xffffffff), got['intc']
    assert check(w7, 'fixture', ch3td=True, intc=True, wake7=True) == got
    # Past the firmware's 10 ms but inside 20 ms: passes, flagged.
    late = w7.replace('at_us=2310', 'at_us=15000').replace('idle_us=2310', 'idle_us=15000').replace(
        'beyond_fw_bound=0', 'beyond_fw_bound=1')
    assert check(late, 'fixture', wake7=True)['teardown']['idle']['beyond_fw_bound'] == 1
    # All-ones everywhere is reported as view=all-ones, not refused.
    ones = w7
    for at in STATE_AT:
        ones = ones.replace(state(at), state(at, 'ffffffff'))
    assert set(check(ones, 'fixture', wake7=True)['intc']['states']['views'].values()) == {'all-ones'}
    for bad in (both,  # a9/aop-wake6 lines
                w7.replace(idle7 + '\n', ''),
                w7.replace('beyond_fw_bound=0 result=idle', 'beyond_fw_bound=0 result=timeout'),
                w7.replace('slices=3/20', 'slices=2/20'),
                w7.replace(idle7.splitlines()[1] + '\n', ''),
                w7.replace('8d0=00000040 idle=0\nSPI0 CH3TD idle-sample slice=1 at_us=2002 c3=02010444',
                           '8d0=00000040 idle=0\nSPI0 CH3TD idle-sample slice=1 at_us=2002 c3=02010446'),
                w7.replace('idle_us=2310', 'idle_us=12000'),
                w7.replace('idle_us=2310', 'idle_us=25000').replace('beyond_fw_bound=0', 'beyond_fw_bound=1'),
                w7.replace(idle7, '\n'.join(idle7.splitlines()[::-1])),
                w7.replace(idle7 + '\n', '').replace('SPI0 CH3TD before ', idle7 + '\nSPI0 CH3TD before '),
                w7.replace(state('pre-ack') + '\n', ''),
                w7.replace(state('post-disable'), state('post-disable', mask='ffefffff')),
                w7.replace(state('post-disable'), state('post-disable').replace('view=intc', 'view=all-ones')),
                w7.replace(state('pre-disable'), state('pre-disable').replace('src_masked=1', 'src_masked=0')),
                w7.replace(state('pre-ack'), state('post-ack')).replace(state('post-ack') + '\nAOP INTC ack',
                                                                         state('pre-ack') + '\nAOP INTC ack'),
                w7.replace(' mask_a80=ffffffff src_masked=1 view=intc result=pass', ' result=pass'),
                w7.replace('AOP INTC pre-ack', state('pre-ack') + '\nAOP INTC pre-ack')):
        try:
            check(bad, 'fixture', wake7=True)
        except AssertionError:
            continue
        raise AssertionError('bad wake7 sequence accepted')
    print('PASS SPI0 reload checker --wake7: idle samples 0..n-1 with only the last idle, one idle-poll summary '
          'within 20 ms (beyond the firmware 10 ms reported), four ordered INTC state lines with 0x10014 masked '
          'after the disable, view=all-ones reported; timeout, count/sequence/flag mismatches, missing, extra, '
          'misordered or unmasked state lines and the wake6 format rejected')


    # Wake8 fixture: SCM stays 3 even though the interrupt reset completes.
    def trace(stage):
        return (f'SPI0 SCM stage={stage} errno=0 ctl=00000011 clk=01b71b00 '
                'scm_lo=00000003 scm_hi=00000000 c3_interval=01507725 c3_cursor=00000000 '
                'c3=00010406 route=00000010 rate=019000c8 c3c=00200009 desc=00000000 '
                '8d0=00000000 8d4=00000000 cpu=halted')
    pulse = '\n'.join((trace('pre-intc-reset'),
        'AOP INTC reset begin pa=210800808 sequence=1/read/0/read cpu=halted',
        'COLD-W step=intc-reset bank=wrapper offset=0808 before=00000000 value=00000001',
        'COLD-R offset=0808 after=00000001',
        'COLD-W step=intc-reset bank=wrapper offset=0808 before=00000001 value=00000000',
        'COLD-R offset=0808 after=00000000', trace('post-intc-reset'),
        'AOP INTC reset errno=0 asserted=00000001 after=00000000 result=pass',
        'COLD-W step=intc-reset bank=wrapper offset=0044 before=00000200 value=00000210'))
    w8 = w7.replace('SPI0 reset errno=0', trace(SCM_STAGES[0]) + '\nSPI0 reset errno=0')
    w8 = w8.replace('SPI0 CH3TD begin', trace(SCM_STAGES[1]) + '\nSPI0 CH3TD begin')
    w8 = w8.replace('SPI0 CH3TD window', trace(SCM_STAGES[2]) + '\n' + trace(SCM_STAGES[3]) + '\nSPI0 CH3TD window')
    w8 = w8.replace('AOP INTC state at=pre-ack', trace(SCM_STAGES[4]) + '\nAOP INTC state at=pre-ack')
    w8 = w8.replace('AOP released', pulse + '\nAOP released')
    assert check(w8, 'fixture', wake8=True)['reset']['scm_final'] == (3, 0)
    assert check(w8.replace('after=00000001', 'after=00000000').replace('asserted=00000001', 'asserted=00000000'),
                 'fixture', wake8=True)['reset']
    for bad in (w7, w8 + '\nAOP crashlog SCMErr:0x3,3',
                w8.replace('AOP INTC reset errno=0 asserted=00000001 after=00000000',
                           'AOP INTC reset errno=0 asserted=00000001 after=00000001'),
                w8.replace('COLD-R offset=0808 after=00000000', ''),
                w8.replace(trace(SCM_STAGES[3]), ''),
                w8.replace(trace(SCM_STAGES[5]), trace(SCM_STAGES[6])),
                w8.replace('offset=0808 before=00000001 value=00000000',
                           'offset=0808 before=00000001 value=00000001'),
                w8.replace(pulse, pulse + '\n' + pulse),
                w8.replace(trace(SCM_STAGES[6]), trace(SCM_STAGES[6]).replace('8d0=00000000', '8d0=00000040')),
                w8.replace('offset=0044 before=00000200 value=00000210',
                           'offset=0044 before=00000200 value=00000200')):
        try:
            check(bad, 'fixture', wake8=True)
        except AssertionError:
            continue
        raise AssertionError('bad wake8 sequence accepted')
    print('PASS SPI0 reload checker --wake8: event-3 pulse/readbacks, seven ordered SCM milestones, SCM=3 allowed; '
          'missing/duplicate/misordered steps, no RUN, stuck reset, post-pulse fault and SCMErr rejected')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dmesg', type=Path, nargs='?')
    ap.add_argument('--entry')
    ap.add_argument('--sys', action='store_true', help='aop_run=1 entry: no IIO expected')
    ap.add_argument('--reason', choices=RUN_REASONS,
                    help='require this decision reason (a kexec after a quiesced run: prev-quiesce-halted)')
    ap.add_argument('--ch3td', action='store_true',
                    help='a9/aop-wake5: require the channel-3 teardown and the 150 ms pre-RUN window')
    ap.add_argument('--intc', action='store_true',
                    help='a9/aop-wake6: require the IRQ 0x10014 disable before the reset and ack before release')
    ap.add_argument('--wake7', action='store_true',
                    help='a9/aop-wake7: --ch3td --intc plus the sliced idle wait and the INTC state snapshots')
    ap.add_argument('--wake8', action='store_true', help='wake7 plus the INTC event-3 reset and seven SCM milestones')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.dmesg or not args.entry:
        ap.error('dmesg and --entry are required')
    result = check(args.dmesg.read_text(errors='replace'), args.entry, not args.sys, args.reason, args.ch3td,
                   args.intc, args.wake7, args.wake8)
    print(f'PASS SPI0 reset reload: decision=run reason={result["reason"]} auto_boot={result["auto_boot"]} '
          f'out4020={result["out4020"]} prev={result["prev"]} steps={result["prev_steps"]}; +0x8d0 {result["before_8d0"]} -> {result["after_8d0"]} '
          f'(bit6 before={result["pending_before"]}), pre-RUN {result["prerun"]}, ps_spi0 {result["pmgr"]}; '
          'accel STARTEP through IIO with no crashlog; check streaming/tilt separately')
    if result['teardown']:
        t = result['teardown']
        print('PASS channel-3 teardown: C3 ' + ' -> '.join(f'{v:08x}' for v in t['c3']) +
              f', C3C {t["c3c"][0]:08x} written {t["c3c"][1]:08x} read {t["c3c"][2]:08x}, '
              f'desc {t["desc"][0]:08x} -> {t["desc"][1]:08x}, '
              f'+0x8d0 {t["fault"][0]:08x} -> {t["fault"][1]:08x}, window 31/31 clear over {t["elapsed_ms"]} ms; '
              '19:50: C3 01011445 -> 01011044 -> 02010006 -> 00010406, C3C 00200029 written / 00200009 read')
    if result['reset']:
        print(f'PASS wake8 INTC event-3 1/read/0/read, final readback 0, seven guarded SPI0/SCM milestones; '
              f'SCM final low/high={result["reset"]["scm_final"]} (observational)')
    if result['intc']:
        n = result['intc']
        print(f'PASS INTC 0x10014: +0xa00 {n["disable"][0]:08x} -> {n["disable"][1]:08x}, '
              f'+0x980 {n["ack"][0]:08x} -> {n["ack"][1]:08x}; '
              'pre-ack scm_lo/hi/interval/cursor/+0x81c ' + ' '.join(f'{v:08x}' for v in n['pre']) +
              ', post-ack ' + ' '.join(f'{v:08x}' for v in n['post']) +
              ' (reported, not gated); STARTEP 0x28 and streaming are the pass criteria')
    if result['teardown'] and result['teardown'].get('idle'):
        i = result['teardown']['idle']
        print(f'PASS channel-3 idle wait: run_before={i["run_before"]} idle after {i["idle_us"]} us in '
              f'{i["slices"]}/20 slices (beyond the firmware 10 ms: {i["beyond_fw_bound"]}); per slice '
              '(at_us c3 scm_lo scm_hi 8d0): ' + ', '.join(' '.join(f'{v:08x}' if k else str(v)
                                                                   for k, v in enumerate(row))
                                                          for row in i['samples']))
    if result['intc'] and result['intc']['states']:
        st = result['intc']['states']
        print('INFO INTC words (808 81c 900 980 a00 a04 a08 a80 c020 c024): ' + '; '.join(
            f'{at} ' + ' '.join(f'{v:08x}' for v in st['words'][at]) + f' view={st["views"][at]}'
            for at in STATE_AT))


if __name__ == '__main__':
    main()
