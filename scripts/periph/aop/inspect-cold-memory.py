#!/usr/bin/env python3
"""Decode frozen forced-cold SRAM. Host only; pointer walks stay in local RAM."""
import argparse
import hashlib
from pathlib import Path
import re
import struct

SIZE = 0x9b000
SPANS = ((0, 0x1000, 'boot'), (0x1000, 0x3a000, 'text-tags'),
         (0x3a000, 0x53940, 'data'), (0x53940, 0x6e000, 'bss'),
         (0x6e000, SIZE, 'scratch'))
MAIN_SHA = 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'
EXT_SHA = '2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22'
BASE = 0x1000000


def word(data, at):
    return struct.unpack_from('<I', data, at)[0]


def runtime_report(after, main):
    """Layouts proved by the SHA-pinned task/PRNG/fatal instructions.

    All dereferences are bounded reads of this file, never hardware addresses.
    A saved context/LR is not a sampled PC. Avoid printing per-device IDs/seeds.
    """
    def local(va, size):
        if va < BASE + 0x3a000 or va + size > BASE + 0x6e000:
            raise ValueError(f'pointer outside captured data RAM: {va:08x}+{size:x}')
        return va - BASE

    def w(va):
        return word(after, local(va, 4))

    def string(va):
        if not BASE <= va < BASE + len(main):
            return '<outside pinned image>'
        return main[va-BASE:va-BASE+128].split(b'\0')[0].decode('ascii', errors='replace')

    coherent = word(after, 0x29c) == 0xe3011001 and word(after, 0x47474) == word(after, 0x389c0)
    lines = [f'Runtime decode: cache_off_guard_agrees={int(coherent)}; no sampled PC; bounded local-file pointers only.']
    table, length = struct.unpack_from('<II', main, 0x48)
    tags = {}
    cursor = table
    while cursor < table + length:
        tag, size = struct.unpack_from('<II', main, cursor)
        tags[tag.to_bytes(4, 'big').decode('ascii')] = after[cursor+8:cursor+8+size]
        cursor += 8 + size
    lines.append('  patchbay: ' + ' '.join(f'{key}_nonzero={int(any(tags[key]))}' for key in ('PRNG', 'ECID', 'NONC')))
    lines.append(f'  PRNG object 01047a80: override_ecid={w(0x1047aa4):08x}{w(0x1047aa0):08x} '
                 f'override_seed_ptr={w(0x1047ab0):08x} override_seed_len={w(0x1047ab4)} device_info={w(0x1047abc):08x}; '
                 'ECID imported to stack via 01038b77; zero is fatal even with a valid PRNG seed.')
    lines.append(f'  fatal stop code 0106ca3c={w(0x106ca3c):08x}; '
                 f'record depth 01052b08={w(0x1052b08)} crash_buffer={w(0x1052b1c):08x}; '
                 f'first type={w(0x1052b34):08x} format={w(0x1052b40):08x} {string(w(0x1052b40))!r}')
    fatal_lr = [off for off in range(0x44c00, 0x46c00, 4) if word(after, off) == 0x103501f]
    if coherent and w(0x106ca3c) == 4 and w(0x1052b40) == 0x1039ac4 and fatal_lr and not any(tags['ECID']):
        at = fatal_lr[0]
        lines.append(f'  panic saved varargs at {BASE+at+4:08x}: AAPCS alignment word skipped; '
                     f'ecid_nonzero={int(bool(word(after, at+8) | word(after, at+12)))} '
                     f'device_info={word(after, at+16):08x}.')
        lines.append('  PRNG FATAL: zero ECID + recorded PRNG !patchbay + saved return after 0103501a BL panic. '
                     'Fatal stop wrote code 4, then 010328d8 loops on 0102760c (DSB/WFI, IRQs disabled). '
                     'No timer/event/doorbell can resume initialization from this non-returning path.')
    lines.append(f'  scheduler: current={w(0x1052a0c):08x} (4=pre-task sentinel); '
                 f'ready_bitmap={w(0x106c754):08x} ready_count={w(0x106c758)}; '
                 f'blocked_head={w(0x10529fc):08x} blocked_tail={w(0x1052a00):08x} '
                 f'second_list_head={w(0x1052a04):08x} second_list_tail={w(0x1052a08):08x}')
    try:
        count, array = w(0x106d280), w(0x106d27c)
        if count > 128:
            raise ValueError('task capacity exceeds bounded decoder limit')
        offset = local(array, count * 4)
        lines.append(f'  task registry 0106d27c: array={array:08x} capacity={count}')
        for i in range(count):
            task = word(after, offset + i * 4)
            if not task:
                continue
            at = local(task, 0x5c)
            context = w(task + 0x28)
            local(context, 0x150)
            priority = after[at+0x14]
            queue = 0x106c75c + priority * 8
            lines.append(f'    task[{i}]={task:08x} name={string(w(task+0x10))!r} '
                         f'id={w(task+8)} priority={priority}/{after[at+0x15]} suspended={after[at+0x16]} '
                         f'wait_object={w(task+0xc):08x} stack={w(task+0x20):08x}+{w(task+0x24):x} '
                         f'context={context:08x} entry={w(context+4):08x} arg={w(context):08x} '
                         f'saved_sp={w(context+0x34):08x} saved_lr={w(context+0x3c):08x}')
            if priority < 32:
                lines.append(f'      ready[{priority}]={w(queue):08x}/{w(queue+4):08x}; '
                             'saved_lr 01029edd is the initial task trampoline, not a live PC.')
    except ValueError as error:
        lines.append(f'  task decode refused: {error}')
    # The management context is the endpoint-0 registry, not a running task list.
    try:
        context = w(0x1052a68)
        local(context, 0x40)
        lines.append(f'  RTKit management={context:08x} state={w(context):x} '
                     f'endpoint_handle={w(context+8):08x} received_version={w(context+0xc):08x} '
                     f'flags={w(context+0x10):08x}; state 1 precedes HELLO negotiation, state 3 follows version acceptance.')
    except ValueError as error:
        lines.append(f'  RTKit decode refused: {error}')
    lines.append(f'  RTKit global 01052ab8 +0xc={w(0x1052ac4):08x}; '
                 'ext 01072202 sets it to 0x21 immediately after the PRNG call returns; '
                 f'WFI mode byte 0106ca38={after[0x6ca38]} (0 selects 01027616 WFI).')
    lines.append(f'  endpoint registry: capacity={w(0x1052a3c)} slot0={w(0x1052a1c):08x}; '
                 f'log PA={w(0x106c8d4):08x}{w(0x106c8d0):08x} size={w(0x106c8d8):x}; '
                 f'crash context at 0106c908 raw+8/+c={w(0x106c910):08x}/{w(0x106c914):08x}. '
                 'The log PA ffffffff/0 is an unallocated sentinel, not a local log ring; no external pointer followed.')
    return '\n'.join(lines) + '\n'


def pinned_report(before, after, main, ext):
    """Profile derived from pinned instructions; all addresses stay in local RAM.

    No live PC is captured. Do not label every saved LR a live frame or call
    lock magics panics. Cache-off captures permit bounded local-file decoding.
    """
    if hashlib.sha256(main).hexdigest() != MAIN_SHA or hashlib.sha256(ext).hexdigest() != EXT_SHA:
        raise ValueError('pinned main/ext SHA256 mismatch')
    if len(before) != SIZE or len(after) != SIZE:
        raise ValueError('incomplete SRAM snapshots')
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
    arm = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    thumb = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    lines = ['Pinned firmware mapping (inferred names, stripped image; linked VAs):']
    for lo, hi, name in (
        (0x3a000, 0x3f000, 'LPAE tables; reset stub +68..+394'),
        (0x3f000, 0x44c00, 'initial allocator arena / constructed work stacks; +285d0'),
        (0x44c00, 0x46c00, 'boot system stack; banked SP constants +27424'),
        (0x46c00, 0x47400, 'banked exception stacks'),
        (0x529a0, 0x529fc, 'cache geometry; +28264'),
        (0x52c28, 0x52ce8, 'mailbox transport; built by +341f0, methods +2ca24'),
        (0x52ce8, 0x52da8, 'mailbox transport with companion +4000 window'),
        (0x52da8, 0x52df8, 'IOP interrupt controller; vtable +39d48 -> +2c524'),
        (0x52df8, 0x52e40, 'timer object; +341f0 selects wrapper+c000'),
        (0x52e40, 0x52eb8, 'platform clock object'),
        (0x52eb8, 0x52f18, 'platform device; vtable +39db4 -> +341f0'),
    ):
        count = sum(before[at:at+4] != after[at:at+4] for at in range(lo, hi, 4))
        lines.append(f'  {BASE+lo:08x}..{BASE+hi:08x} changed_words={count}: {name}')
    lines.append('Selected AP-visible backing words (may be stale with AOP D-cache on):')
    for off, count, name in (
        (0x47474, 1, 'runtime guard'), (0x47510, 5, 'ext mapping object / EC0p at +8'),
        (0x529fc, 6, 'scheduler globals incl current task at +52a0c'),
        (0x52a3c, 2, 'endpoint registry count/array'),
        (0x52a68, 4, 'management context list head/tail, log context list head/tail'),
        (0x52c28, 6, 'mailbox base/flags'), (0x52ce8, 6, 'companion mailbox base/flags'),
        (0x52da8, 6, 'interrupt controller base/flags'),
        (0x6c8a8, 8, 'diagnostic callback state passed by +341f0'),
        (0x6c8c8, 16, 'log endpoint context; +32682/+326f0, buffer at +8/+c'),
        (0x6c908, 8, 'crash endpoint context registered as endpoint 4'),
    ):
        values = ' '.join(f'{word(after, off+i*4):08x}' for i in range(count))
        lines.append(f'  +{off:05x} {name}: {values}')
    tag = word(after, 0x389c0)
    hits = [off for off in range(0x44c00, 0x46c00, 4) if word(after, off) == tag]
    if tag != word(main, 0x47474) and hits and word(after, 0x47474) == word(main, 0x47474):
        lines.append('CACHE EVIDENCE: patched STKG is on the boot stack at ' +
                     ', '.join(f'+{off:x}' for off in hits) +
                     ' but guard backing is still the image default. C entry/guard import ran; '
                     'backing globals cannot be treated as coherent execution state.')
    # A saved Thumb return must follow an actual BL/BLX ending at that VA.
    # Retain every candidate, not a fabricated current-PC backtrace.
    lines.append('Boot-stack return candidates validated against local instruction bytes (not live frames):')
    ext_returns = []
    for off in range(0x44c00, 0x46c00, 4):
        value = word(after, off)
        if not value & 1:
            continue
        target = value & ~1
        image, origin = (main, BASE) if BASE <= target < BASE+0x3a000 else (ext, BASE+0x6e000)
        relative = target-origin
        if not 4 <= relative <= len(image):
            continue
        calls = []
        for size in (4, 2):
            insns = list(thumb.disasm(image[relative-size:relative], target-size))
            if len(insns) == 1 and insns[0].mnemonic in ('bl', 'blx') and insns[0].size == size:
                calls.append(insns[0])
        if not calls:
            continue
        call = calls[0]
        lines.append(f'  +{off:05x} saved={value:08x} call={call.address:08x} {call.mnemonic} {call.op_str}')
        if image is ext:
            ext_returns.append(value)
    if ext_returns:
        lines.append('EXT EVIDENCE: an ext-code return is present alongside the new STKG. '
                     'Supports execution from staged ext; does not prove every ext page or later DMA is accessible.')
    lines.append('Reset cache-enable instruction: ' + '; '.join(
        f'{i.address:08x} {i.mnemonic} {i.op_str}' for i in arm.disasm(main[0x298:0x2b0], BASE+0x298)))
    for magic in (b'CLHE', b'CLHT', b'CLSD', b'PANIC', b'panic:'):
        hits = [m.start() for m in re.finditer(re.escape(magic), after)
                if before[m.start():m.end()] != after[m.start():m.end()]]
        lines.append(f'Changed crash/log signature {magic!r}: ' +
                     (', '.join(f'+{off:x}' for off in hits) if hits else 'none'))
    lines.append('No signature match does not exclude a cached/other-format crash log. '
                 'deadc0de and b0bed0ff are lock-state magics in +285xx, not panic evidence.')
    return '\n'.join(lines) + '\n' + runtime_report(after, main)


def inspect(before, after):
    if len(before) != SIZE or len(after) != SIZE:
        raise ValueError(f'need two complete {SIZE:#x}-byte snapshots; got {len(before):#x}/{len(after):#x}')
    lines = ['Frozen SRAM at AP PA 0x210e00000; samples are not atomic.',
             'Changes establish memory activity; source, PC and firmware identity need corroboration.']
    for name, data in (('before', before), ('after', after)):
        guard = struct.unpack_from('<I', data, 0x47474)[0]
        tag_guard = struct.unpack_from('<I', data, 0x389c0)[0]
        lines.append(f'{name}: sha256={hashlib.sha256(data).hexdigest()} guard={guard:08x} STKG={tag_guard:08x}')
    for start, end, name in SPANS:
        words = [at for at in range(start, end, 4) if before[at:at+4] != after[at:at+4]]
        lines.append(f'{name}: changed_words={len(words)} span={start:#x}..{end:#x}')
        for at in words[:32]:
            old, new = (struct.unpack_from('<I', data, at)[0] for data in (before, after))
            lines.append(f'  +{at:05x} {old:08x} -> {new:08x}')
        if len(words) > 32:
            lines.append(f'  ... {len(words)-32} further changed words retained in raw capture')
        # Candidate text only, never claim an identified RTKit log/crash layout.
        shown = 0
        for match in re.finditer(rb'[\x20-\x7e]{8,}', after[start:end]):
            lo, hi = start + match.start(), start + match.end()
            if before[lo:hi] != after[lo:hi]:
                lines.append(f'  changed_text +{lo:05x}: {match.group()[:160].decode("ascii")}')
                shown += 1
                if shown == 8:
                    break
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--firmware', type=Path, help='pinned main for instruction/structure mapping')
    parser.add_argument('--ext', type=Path, help='pinned external image (required with --firmware)')
    parser.add_argument('--changed-csv', type=Path, help='write every changed word, no truncation')
    args = parser.parse_args()
    if bool(args.firmware) != bool(args.ext):
        parser.error('--firmware and --ext must be supplied together')
    before = (args.capture / 'cold_before.bin').read_bytes()
    after = (args.capture / 'cold_after.bin').read_bytes()
    print(inspect(before, after), end='')
    if args.firmware:
        print(pinned_report(before, after, args.firmware.read_bytes(), args.ext.read_bytes()), end='')
    if args.changed_csv:
        import csv
        with args.changed_csv.open('w', newline='') as output:
            writer = csv.writer(output)
            writer.writerow(('offset', 'linked_va', 'before', 'after'))
            for off in range(0, SIZE, 4):
                if before[off:off+4] != after[off:off+4]:
                    writer.writerow((f'0x{off:x}', f'0x{BASE+off:x}',
                                     f'0x{word(before, off):08x}', f'0x{word(after, off):08x}'))


if __name__ == '__main__':
    main()
