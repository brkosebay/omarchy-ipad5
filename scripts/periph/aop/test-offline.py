#!/usr/bin/env python3
"""Host-only malformed-input and extraction checks; no device connection."""
from pathlib import Path
import argparse
import os
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--active-source', type=Path,
                    default=HERE / 'kernel/apple-a9-aop-active.c',
                    help='extract production helpers from a saved driver revision for regression replay')
    args = ap.parse_args()
    m = runpy.run_path(str(HERE / 'extract-firmware.py'))
    raw = (ROOT / 'build/periph/aop-r1/ipsw/20H350__iPad6,11/aopfw-ipad6faop.im4p').read_bytes()
    assert m['digest'](raw) == m['COMPONENT_SHA']
    bundle_bytes = m['im4p_payload'](raw)
    assert m['digest'](bundle_bytes) == m['BUNDLE_SHA']
    bundle = m['OSSerialize'](bundle_bytes).bundle()
    assert [(e['header']['Target'], len(e['data'])) for e in bundle['firmware']] == [
        ('ipad6faop', 342336), ('ipad6faop_ext', 37024)]
    assert bundle['firmware'][1]['header']['Address'] == '0x0106e000'
    assert m['digest'](bundle['firmware'][0]['data']) == 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'
    assert m['digest'](bundle['firmware'][1]['data']) == '2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22'
    bad = [b'', bundle_bytes[:-1], bundle_bytes + b'\0\0\0\0',
           b'\xd3\0\0\0' + struct.pack('<I', 0x8c000000),
           b'\xd3\0\0\0' + struct.pack('<II', 0x81000001, 0x8c000000)]
    for value in bad:
        try:
            m['OSSerialize'](value).bundle()
        except ValueError:
            pass
        else:
            raise AssertionError('malformed bundle accepted')
    for value in (b'', raw[:-1], raw + b'\0', b'\x30\x80'):
        try:
            m['im4p_payload'](value)
        except ValueError:
            pass
        else:
            raise AssertionError('malformed IM4P accepted')
    with tempfile.TemporaryDirectory(prefix='aop-offline-') as tmp:
        tmp = Path(tmp)
        inc = tmp / 'linux'
        inc.mkdir()
        (inc / 'types.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16;
typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64;
''')
        (inc / 'errno.h').write_text('''#pragma once
#define ENOENT 2
#define E2BIG 7
#define EINTR 4
#define EAGAIN 11
#define EACCES 13
#define EFAULT 14
#define EINVAL 22
#define ENOSPC 28
#define ERANGE 34
#define EPROTO 71
#define EILSEQ 84
#define EMSGSIZE 90
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EREMOTEIO 121
''')
        (inc / 'string.h').write_text('#include <string.h>\n')
        (inc / 'unaligned.h').write_text('''#pragma once
#include <linux/types.h>
static inline u16 get_unaligned_le16(const u8 *p) { return (u16)p[0] | (u16)p[1]<<8; }
static inline u32 get_unaligned_le32(const u8 *p) { return (u32)get_unaligned_le16(p) | (u32)get_unaligned_le16(p+2)<<16; }
static inline u64 get_unaligned_le64(const u8 *p) { return (u64)get_unaligned_le32(p) | (u64)get_unaligned_le32(p+4)<<32; }
static inline void put_unaligned_le16(u16 x, u8 *p) { p[0]=x; p[1]=x>>8; }
static inline void put_unaligned_le32(u32 x, u8 *p) { put_unaligned_le16(x,p); put_unaligned_le16(x>>16,p+2); }
static inline void put_unaligned_le64(u64 x, u8 *p) { put_unaligned_le32(x,p); put_unaligned_le32(x>>32,p+4); }
''')
        # Compile the production E1 walk/copy/accounting functions with a fake
        # RAM backend. No rewritten simulation of the scanner loop.
        reader = (HERE / 'kernel/a9-aop-ring-reader.c').read_text()
        functions = []
        for name in ('static bool rr_linear_page(', 'static bool rr_contains_page(',
                     'static int rr_scan_page_reason(', 'static int rr_cma_page(',
                     'static enum a9_rr_range_class rr_page_class(',
                     'static enum a9_rr_range_class rr_hit_class(',
                     'static void rr_skip_page(', 'static int rr_scan_chunk(',
                     'static int rr_scan_copy(', 'static int rr_scan_range('):
            start = reader.index(name)
            end = reader.index('\n}\n', start) + 3
            functions.append(reader[start:end])
        (tmp / 'ring-scan-functions.h').write_text('\n'.join(functions))
        target = (HERE / 'kernel/a9-aop-ring-target.c').read_text()
        observer = (HERE / 'kernel/apple-a9-aop.c').read_text()
        functions = []
        names = ('bool a9_aop_ring_pa_own_page(', 'static bool rt_stack_page(',
                 'static bool rt_bad_page(', 'static enum a9_rt_reason rt_page_info(',
                 'static int rt_read_chunk(', 'static void rt_take(',
                 'static void rt_capture_ranges(', 'void a9_aop_ring_pa_capture(',
                 'static bool rt_signature(')
        for source, selected in ((observer, ('bool apple_a9_aop_ring_mode(',
                                             'bool apple_a9_aop_ring_pa_enabled(',
                                             'int apple_a9_aop_ring_pa_args(')),
                                 (target, names)):
            for name in selected:
                start = source.index(name)
                end = source.index('\n}\n', start) + 3
                functions.append(source[start:end])
        (tmp / 'ring-target-functions.h').write_text('\n'.join(functions))
        # Exercise the actual PMGR guard and state callback together. The fake
        # MMIO backend asserts all PMGR reads and locks precede wrapper reads.
        functions = [(HERE / 'kernel/pmgr-observe.inc').read_text().split(
            'EXPORT_SYMBOL_GPL', 1)[0]]
        for name in ('struct a9_aop {', 'struct a9_aop_snapshot {'):
            start = observer.index(name)
            functions.append(observer[start:observer.index('\n};', start) + 3])
        for name in ('bool apple_a9_aop_state_probe(', 'static void a9_aop_read_state(',
                     'static void a9_aop_state_format(', 'static int a9_aop_capture_state(',
                     'static int a9_aop_state_show(', 'unsigned int apple_a9_aop_attach_stage(',
                     'static void a9_aop_read_attach_status(', 'static int a9_aop_capture_attach(',
                     'static int a9_aop_attach_status_show('):
            start = observer.index(name)
            functions.append(observer[start:observer.index('\n}\n', start) + 3])
        (tmp / 'state-probe-functions.h').write_text('\n'.join(functions))
        active = args.active_source.read_text()
        constants = active[active.index('#define AOP_CPU_CONTROL'):active.index('/* Firmware: linked')]
        types = active[active.index('static const u32 a9_exp_reg_offsets'):active.index('struct a9_aop {')]
        (tmp / 'mailbox-types.h').write_text(constants + types)
        functions = []
        for name in ('static void a9_aop_experiment_regs(', 'static int a9_aop_send(',
                     'static void a9_aop_experiment_unmask(',
                     'static void a9_aop_experiment_wake('):
            start = active.index(name)
            functions.append(active[start:active.index('\n}\n', start) + 3])
        (tmp / 'mailbox-functions.h').write_text('\n'.join(functions))
        start = active.index('static int a9_aop_pool_claim(')
        (tmp / 'pool-functions.h').write_text(active[start:active.index('\n}\n', start) + 3])
        functions = []
        for name in ('static void a9_aop_ping_tick(', 'static void a9_aop_mgmt(', 'static void a9_aop_getbuf(',
                     'static bool a9_aop_system_buffers_ready(',
                     'static void a9_aop_system_buffer(', 'static void a9_aop_system_rx(',
                     'static int a9_aop_system_start(',
                     'static void a9_aop_experiment_window(',
                     'static bool a9_aop_experiment_handshake('):
            start = active.index(name)
            functions.append(active[start:active.index('\n}\n', start) + 3])
        start = active.index('#define a9_aop_wait(')
        wait = active[start:active.index('\n/* ---- EPIC', start)]
        functions.insert(0, wait)
        (tmp / 'experiment-functions.h').write_text('\n'.join(functions))
        (tmp / 'stage3-wait.h').write_text(wait)
        # Compile the full continuation and its actual protocol callbacks.
        # Only the MMIO/firmware/IIO boundaries are replaced by test doubles.
        types = active[active.index('enum a9_aop_phase {'):active.index('struct a9_aop_trace {')]
        constants = active[active.index('#define AOP_DOMAINS'):active.index('static const char * const domain_names')]
        functions = []
        for name in ('static void a9_aop_finish(', 'static bool a9_aop_dead(',
                     'static struct a9_aop_ep *a9_aop_find_ep(',
                     'static void a9_aop_mgmt(', 'static void a9_aop_ping_tick(',
                     'static bool a9_aop_system_buffers_ready(',
                     'static void a9_aop_system_buffer(', 'static void a9_aop_system_rx(',
                     'static void a9_aop_ep_fail(', 'static void a9_aop_getbuf(',
                     'static void a9_aop_ring_desc(', 'static void a9_aop_record_service(',
                     'static void a9_aop_report(', 'static void a9_aop_epic(',
                     'static void a9_aop_rx_ring(', 'static void a9_aop_afk(',
                     'static int a9_aop_queue_property(', 'static int a9_aop_property(',
                     'static int a9_aop_property32(', 'static int a9_aop_set_rate(',
                     'static int a9_aop_send_calibration(', 'static int a9_aop_system_start('):
            start = active.index(name)
            functions.append(active[start:active.index('\n}\n', start) + 3])
        start = active.index('start_endpoints:\n', active.index('static void a9_aop_boot_work('))
        continuation = active[start:active.index('\n}\n', start) + 3]
        functions.append('''static void continue_boot(struct a9_aop *aop) {
struct a9_aop_accel_config config = {};
struct a9_spu_subpacket first = {};
struct iio_dev *iio;
unsigned int i;
int ret;
goto start_endpoints;
''' + continuation)
        (tmp / 'stage3-types.h').write_text(constants + types)
        (tmp / 'stage3-functions.h').write_text('\n'.join(functions))
        functions = []
        for name in ('static bool a9_aop_apps_stopped(', 'static int a9_aop_quiesce_afk(',
                     'static int a9_aop_quiesce(', 'static void a9_aop_shutdown('):
            if name not in active:
                continue
            start = active.index(name)
            functions.append(active[start:active.index('\n}\n', start) + 3])
        (tmp / 'shutdown-functions.h').write_text('\n'.join(functions))
        # aop_handoff=sleep: the production record, validation, wake and
        # adoption code, compiled beside the shutdown functions above.
        if 'static bool a9_aop_handoff_wake(' in active:
            start = active.index('struct a9_aop_handoff_live {')
            functions = [active[start:active.index('\n};\n', start) + 4]]
            for name in ('static void a9_aop_handoff_read(', 'static void a9_aop_handoff_digest(',
                         'static struct a9_ho_expect a9_aop_handoff_expect(',
                         'static enum a9_ho_reason a9_aop_handoff_validate(',
                         'static int a9_aop_handoff_inputs(', 'static void a9_aop_handoff_consume(',
                         'static int a9_aop_handoff_resume_power(', 'static void a9_aop_handoff_fail(',
                         'static void a9_aop_handoff_adopt_session(',
                         'static void a9_aop_handoff_classify(', 'static int a9_aop_wrapper_wake(',
                         'static bool a9_aop_handoff_wake(',
                         'static int a9_aop_handoff_write(', 'static int a9_aop_handoff_idle('):
                if name not in active and name == 'static int a9_aop_wrapper_wake(':
                    continue  # before a9/aop-wake
                start = active.index(name)
                functions.append(active[start:active.index('\n}\n', start) + 3])
            (tmp / 'handoff-functions.h').write_text('\n'.join(functions))
            start = active.index('/* ---- reserved shutdown transcript')
            (tmp / 'slog-functions.h').write_text(active[start:active.index('/* RTKit crashlog:', start)])
            (tmp / 'handoff-wrapper.h').write_text(
                active[active.index('#define AOP_CPU_CONTROL'):active.index('/* Firmware: linked')])
        capture = runpy.run_path(str(HERE / 're-property-reply.py'))
        (tmp / 'stage3-capture.h').write_text(capture['fixture']())
        cold_capture = runpy.run_path(str(HERE / 're-calibration.py'))
        (tmp / 'stage3-cal-capture.h').write_text(cold_capture['fixture']())
        stream_capture = runpy.run_path(str(HERE / 're-stream.py'))
        (tmp / 'stage3-stream-capture.h').write_text(stream_capture['fixture']())
        cc = os.environ.get('CC', shutil.which('clang') or 'cc')
        image = ROOT / 'build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin'
        if not image.exists():
            image.parent.mkdir(parents=True, exist_ok=True)
            image.write_bytes(bundle['firmware'][0]['data'])
        ext_image = tmp / 'ipad6faop_ext-20H350.bin'
        ext_image.write_bytes(bundle['firmware'][1]['data'])
        calibration = tmp / 'accel-calibration.bin'
        dump = ROOT / 'build/firmware/private/IODeviceTree.txt'
        if dump.exists():
            subprocess.run([sys.executable, '-B', str(HERE / 'make-calibration.py'),
                            '--dump', str(dump), '--output', str(calibration)],
                           check=True, stdout=subprocess.DEVNULL)
        cal_arg = str(calibration) if calibration.exists() else '-'
        for test, args in (('protocol', []), ('afk', [str(image), cal_arg]),
                           ('takeover', [str(image), str(ext_image)]), ('diagnostics', []),
                           ('memory', []), ('ring', []), ('ring-scan', []), ('ring-target', []),
                           ('state-probe', []), ('experiment', []), ('stage3', [cal_arg]),
                           ('mailbox', []), ('pool', []), ('shutdown', [cal_arg]),
                           ('handoff', [cal_arg])):
            if test == 'shutdown' and 'static int a9_aop_quiesce(' not in active:
                continue
            if test == 'handoff' and 'static bool a9_aop_handoff_wake(' not in active:
                continue
            if test == 'handoff':
                args = args + [str(tmp / 'handoff-transcript.txt')]
            executable = tmp / f'{test}-test'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g0', '-I', str(tmp),
                            '-I', str(HERE / 'kernel'), str(HERE / f'tests/{test}.c'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable), *args], check=True)
        transcript = tmp / 'handoff-transcript.txt'
        if transcript.exists():
            # The real checkers accept the replayed production transcripts.
            handoff_check = runpy.run_path(str(HERE / 'check-handoff.py'))
            handoff_check['check_previous'](transcript.read_text(), 'aop-handoff-accel-0123456789')
            warm = runpy.run_path(str(HERE / 'check-warm-shutdown.py'))
            warm['check'](Path(str(transcript) + '.quiesce').read_text(), 'aop-handoff-accel-0123456789')
            print('PASS checkers accept the replayed production handoff and quiesce transcripts')
        mc = runpy.run_path(str(HERE / 'make-calibration.py'))
        sample = '''  +-o accel  <class AppleSPUHIDInterface, id 1>
      "accel-orientation" = <01000000ff00000000ff00000000ff00>
      "low-temp-accel-offset" = <0102>
  +-o gyro  <class AppleSPUHIDInterface, id 2>
      "accel-sensitivity-calibration" = <ff>
'''
        data, entries = mc['build'](mc['accel_properties'](sample))
        assert [(k, len(b)) for _, k, b in entries] == [(0x1a, 2), (0x18, 16)], entries
        assert data[:12] == b'A9AC' + struct.pack('<II', 1, 2)
    runpy.run_path(str(HERE / 'decode-crashlog.py'))['self_test']()
    runpy.run_path(str(HERE / 'check-handoff.py'))['self_test']()
    if (HERE / 'check-spi0-reload.py').exists():
        runpy.run_path(str(HERE / 'check-spi0-reload.py'))['self_test']()
    runpy.run_path(str(HERE / 'check-warm-shutdown.py'))['self_test']()
    runpy.run_path(str(HERE / 'tests/force-cold.py'), run_name='__main__')
    runpy.run_path(str(HERE / 'tests/prepare.py'), run_name='__main__')
    runpy.run_path(str(HERE / 'tests/prepare-ring.py'), run_name='__main__')
    runpy.run_path(str(HERE / 'tests/capture.py'), run_name='__main__')
    inspect = runpy.run_path(str(HERE / 'inspect-fingerprint.py'))
    headers = inspect['describe'](image.read_bytes())
    assert len(headers) == 1, headers
    header = headers[0]
    assert (header['header'], header['version'], header['build_offset'], header['table'],
            header['table_size']) == (0x20, 5, 0x47440, 0x389b8, 0x1df)
    assert header['build'] == 'ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465'
    assert header['uuid'] == 'cce47592d067374e8b46151be730c1de'
    for bad in (b'', b'uuid' + b'\xff' * 64, image.read_bytes()[:0x60]):
        assert not inspect['describe'](bad)
    old = 'head 00000000: 00 00 03 00\nbuild@47440 00000000: 69 95 7e 1a\n'
    new = 'window=uuid-header region_offset=0x20 bytes=0x40\nactual 00000000: 00 00 1b 00  ....\n'
    assert inspect['captured_windows'](old) == {0: bytes.fromhex('00000300'), 0x47440: bytes.fromhex('69957e1a')}
    assert inspect['captured_windows'](new) == {0x20: bytes.fromhex('00001b00')}
    evidence = inspect['capture_evidence'](old + new)
    assert all(not window['complete'] for window in evidence['windows'])
    complete = 'window=build-string region_offset=0x47440 bytes=0x40\n' + ''.join(
        f'actual {offset:08x}: ' + ' '.join(['ab'] * 16) + '  ................\n'
        for offset in range(0, 64, 16))
    evidence = inspect['capture_evidence'](complete)
    assert evidence['windows'][2]['complete'] and not evidence['conflicting_bytes']
    conflict = complete + '== patchbay\nbuild@47440 00000000: 00 00 00 00\n'
    evidence = inspect['capture_evidence'](conflict)
    assert evidence['conflicting_bytes'] == 4 and not evidence['windows'][2]['complete']
    evidence = inspect['capture_evidence'](complete + 'build@47440 00000000: 00 00 00 00\n')
    assert evidence['conflicting_bytes'] == 4 and not evidence['windows'][2]['complete']
    r6_capture = ROOT / 'docs/periph/aop-r6-device-debugfs.txt'
    if r6_capture.exists():
        evidence = inspect['capture_evidence'](r6_capture.read_text())
        assert [w['available'] for w in evidence['windows']] == [110, 64, 64, 0]
        assert not evidence['conflicting_bytes']
    print('PASS: independently derived pinned UUID/build/TLV offsets, malformed file metadata, old/new capture parser')
    print('PASS: pinned IM4P/bundle/images; bounded deserialization and malformed-input refusal; calibration generator')


if __name__ == '__main__':
    main()
