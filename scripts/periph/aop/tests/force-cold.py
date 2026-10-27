#!/usr/bin/env python3
"""Build actual force-stop/drain/cold-loader functions with fake MMIO."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'kernel/apple-a9-aop-active.c'

def main():
    source = SOURCE.read_text()
    with tempfile.TemporaryDirectory(prefix='aop-cold-') as tmp:
        tmp = Path(tmp)
        inc = tmp / 'linux'
        inc.mkdir()
        (inc / 'types.h').write_text('#include <stdint.h>\n#include <stddef.h>\n#include <stdbool.h>\ntypedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64;\n')
        # Reuse the maintained errno/unaligned shims from the full test runner.
        runner = (HERE.parent / 'test-offline.py').read_text()
        start = runner.index("        (inc / 'errno.h').write_text(")
        end = runner.index('        # Compile the production', start)
        exec(compile('if True:\n' + runner[start:end], '<shims>', 'exec'), {'inc': inc})
        start = source.index('static int a9_aop_prepare_identity(')
        (tmp / 'identity-functions.h').write_text(source[start:source.index('\n}\n', start) + 3])
        exe = tmp / 'cold-identity'
        subprocess.run([os.environ.get('CC', shutil.which('clang') or 'cc'), '-std=gnu11',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g0',
                        '-I', str(tmp), '-I', str(HERE.parent / 'kernel'), str(HERE / 'cold-identity.c'),
                        '-o', str(exe)], check=True)
        root = HERE.parents[3]
        subprocess.run([str(exe), *[str(root / 'build/periph/aop-r2/firmware/apple/aop' / name)
                        for name in ('ipad6faop-20H350.bin', 'ipad6faop_ext-20H350.bin')]], check=True)
        (tmp / 'cold-constants.h').write_text(source[source.index('#define AOP_REMAP_PA_LO'):source.index('/* Firmware: linked')])
        with (tmp / 'cold-constants.h').open('a') as out:
            out.write(source[source.index('#define AOP_COLD_HELLO_MS'):source.index('#define AOP_EPMAP_MS')])
        functions = []
        # aop_spi0_reset=1 / aop_wake_wrapper=1 helpers exist from a9/aop-wake on.
        spi0 = ('static void a9_aop_spi0_snapshot(', 'static void a9_aop_spi0_reset_locked(',
                'static void a9_aop_spi0_ch3_snapshot(', 'static void a9_aop_spi0_trace_read(',
                'static int a9_aop_spi0_trace(', 'static void a9_aop_spi0_ch3_locked(',
                'static void a9_aop_spi0_pmgr_on(', 'static int a9_aop_spi0_pmgr(',
                'static int a9_aop_spi0_ch3_teardown(', 'static void a9_aop_intc_snapshot(',
                'static int a9_aop_intc_disable(',
                'static int a9_aop_spi0_prereset(', 'static void a9_aop_spi0_fault_read(',
                'static int a9_aop_spi0_recheck(', 'static void a9_aop_spi0_scm_read(',
                'static int a9_aop_intc_ack(', 'static int a9_aop_intc_reset(')
        wake = ('static int a9_aop_wrapper_wake(',)
        if spi0[0] not in source:
            spi0 = wake = ()
        for name in ('static inline int aop_wr(', 'static int a9_aop_control_write(',
                     'static bool a9_aop_force_stopped(', 'static int a9_aop_force_stop(', 'static int a9_aop_force_drain(',
                     *spi0, 'static int a9_aop_cold_hello(', 'static int a9_aop_cold_start(', *wake):
            # Skip forward declarations.
            start = source.index(name)
            if source.find(';', start) < source.find('{', start):
                start = source.index(name, start + len(name))
            functions.append(source[start:source.index('\n}\n', start) + 3])
        (tmp / 'cold-functions.h').write_text('\n'.join(functions))
        # The real retained transcript the 15:52 SPI0 reload replayed (AOP-PREV),
        # rebuilt byte-exact: the header said lines=63 bytes=3479.
        root = HERE.parents[3]
        log = root / 'docs/periph/logs/aop-spi0-20261026/dmesg-after.txt'
        prefix = 'apple-a9-aop-active 210800000.aop: AOP-PREV '
        lines = [line.split(prefix, 1)[1] for line in log.read_text().splitlines()
                 if prefix in line and 'AOP-PREV shutdown transcript' not in line]
        text = ''.join(line + '\n' for line in lines)
        assert len(lines) == 63 and len(text.encode()) == 3479, (len(lines), len(text))
        assert '"' not in text and '\\' not in text
        (tmp / 'spi0-transcript.h').write_text('static const char spi0_transcript[] =\n' + ''.join(
            f'\t"{line}\\n"\n' for line in lines) + ';\n')
        exe = tmp / 'force-cold'
        subprocess.run([os.environ.get('CC', shutil.which('clang') or 'cc'), '-std=gnu11',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g0',
                        '-I', str(tmp), '-I', str(HERE.parent / 'kernel'), str(HERE / 'force-cold.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        functions = []
        for name in ('static bool a9_aop_ramoops_ready(', 'static int a9_aop_cold_checkpoint(',
                     'static ssize_t cold_release_store('):
            start = source.index(name)
            functions.append(source[start:source.index('\n}\n', start) + 3])
        (tmp / 'trigger-functions.h').write_text('\n'.join(functions))
        exe = tmp / 'cold-trigger'
        subprocess.run([os.environ.get('CC', shutil.which('clang') or 'cc'), '-std=gnu11',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-fsanitize=address,undefined', '-g0', '-pthread',
                        '-I', str(tmp), str(HERE / 'cold-trigger.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        types = source[source.index('static const struct {\n\tconst char *name;'):source.index('enum a9_aop_phase')]
        (tmp / 'snapshot-types.h').write_text(types)
        functions = []
        for name in ('static void a9_aop_cold_read(', 'static int a9_aop_cold_capture('):
            start = source.index(name)
            functions.append(source[start:source.index('\n}\n', start) + 3])
        (tmp / 'snapshot-functions.h').write_text('\n'.join(functions))
        exe = tmp / 'cold-memory'
        subprocess.run([os.environ.get('CC', shutil.which('clang') or 'cc'), '-std=gnu11',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g0',
                        '-I', str(tmp), '-I', str(HERE.parent / 'kernel'), str(HERE / 'cold-memory.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        import runpy
        inspector = runpy.run_path(str(HERE.parent / 'inspect-cold-memory.py'))
        inspect = inspector['inspect']
        baseline = bytes(0x9b000)
        changed = bytearray(baseline)
        changed[0x47474] = 1
        changed[0x6e000:0x6e00b] = b'test panic!'
        report = inspect(baseline, bytes(changed))
        assert 'changed_text +6e000: test panic!' in report
        assert 'data: changed_words=1' in report and 'scratch: changed_words=3' in report
        for a, b in ((baseline[:-1], baseline), (baseline, baseline + b'\0')):
            try:
                inspect(a, b)
            except ValueError:
                pass
            else:
                raise AssertionError('accepted incomplete/oversize capture')
        root = HERE.parents[3]
        capture = root / 'build/periph/aop-stage23/cold2-hello-v3-20261025T130758Z'
        fw = root / 'build/periph/aop-r2/firmware/apple/aop'
        main, ext = [(fw / name).read_bytes() for name in ('ipad6faop-20H350.bin', 'ipad6faop_ext-20H350.bin')]
        if (capture / 'cold_before.bin').exists():
            before, after = [(capture / name).read_bytes() for name in ('cold_before.bin', 'cold_after.bin')]
            report = inspector['pinned_report'](before, after, main, ext)
            assert 'CACHE EVIDENCE:' in report and '+46b84' in report
            assert 'call=010721f8 blx r6' in report and 'EXT EVIDENCE:' in report
            assert 'changed_words=570: LPAE' in report
            try:
                inspector['pinned_report'](before, after, main[:-1], ext)
            except ValueError:
                pass
            else:
                raise AssertionError('accepted unpinned firmware')
            print('PASS pinned cold2v3 profile: exact changed-table count, STKG cache contradiction, validated ext BLX and image refusal')
        for name in ('cold2c-v4-20261025T132921Z', 'cold2b-v4-20261025T133030Z'):
            capture = root / 'build/periph/aop-stage23' / name
            if not (capture / 'cold_before.bin').exists():
                continue
            before, after = [(capture / filename).read_bytes() for filename in ('cold_before.bin', 'cold_after.bin')]
            report = inspector['pinned_report'](before, after, main, ext)
            for expected in ('PRNG FATAL:', 'current=00000004', 'ready_bitmap=02000000',
                             "name='rtk_ep_work'", 'wait_object=00000000', 'format=01039ac4'):
                assert expected in report, expected
            corrupt = bytearray(after)
            corrupt[0x6d27c:0x6d280] = b'\xff'*4
            assert 'task decode refused:' in inspector['runtime_report'](corrupt, main)
            corrupt[0x6ca3c:0x6ca3c+4] = bytes(4)
            assert 'PRNG FATAL:' not in inspector['runtime_report'](corrupt, main)
        identity = __import__('runpy').run_path(str(HERE.parent / 'make-boot-ecid.py'))['extract']
        sample = '+-o chosen  <class IODeviceTreeEntry>\n "unique-chip-id" = <0102030405060708>\n+-o other  <class X>\n "unique-chip-id" = <ff>'
        assert identity(sample) == bytes(range(1, 9))
        for bad in ('', sample.replace('chosen', 'other'), sample.replace('0102030405060708', '00'),
                    sample.replace('0102030405060708', '0000000000000000'), sample + '\n' + sample):
            try:
                identity(bad)
            except ValueError:
                pass
            else:
                raise AssertionError('accepted absent/invalid/ambiguous ECID')
        print('PASS cache-off fatal/task/RTKit decoder, corrupt-pointer refusal, private identity exporter')
        print('PASS frozen SRAM inspector: changed strings/spans and exact-size refusal')

if __name__ == '__main__':
    main()
