#!/usr/bin/env python3
"""Execute the actual kernel entry functions with mocked hardware, never a device.

Checks the consequential boundary: unsupported/reset-based configurations must
not execute WFI, and the allowed experiment must preserve every other bit.
This does not emulate Twister sleep, register accessibility or physical wake.
Run inside the build container with --source drivers/cpuidle/cpuidle-apple-a9-idle.c.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t u64;
#define __cpuidle
#define BIT_ULL(n) (UINT64_C(1) << (n))
#define GENMASK_ULL(h, l) ((UINT64_MAX >> (63 - (h))) & (UINT64_MAX << (l)))
#define sys_reg(a, b, c, d, e) (d)
#define ARM_CPU_IMP_APPLE 0x61
#define APPLE_CPU_PART_TSMC_TWISTER 5
#define module_param(...)
#define MODULE_PARM_DESC(...)
struct cpuidle_device { int unused; };
struct cpuidle_driver { int unused; };
static u64 cfg, cyc, original, written[2];
static unsigned int implementor, part, reads, writes, waits, barriers;
static unsigned int read_cpuid_implementor(void) { return implementor; }
static unsigned int read_cpuid_part_number(void) { return part; }
static u64 mock_read(unsigned int reg) {
    reads++;
    assert(reg == 4 || reg == 5);
    return reg == 4 ? cfg : cyc;
}
static void mock_write(u64 value, unsigned int reg) {
    assert(reg == 5); /* Never write CPU_CFG. */
    assert(writes < 2);
    written[writes++] = value;
    cyc = value;
}
static void cpu_do_idle(void) {
    assert(!(cfg & BIT_ULL(24)));
    assert((cyc & (UINT64_C(3) << 24)) == BIT_ULL(25));
    waits++;
}
#define read_sysreg_s(reg) mock_read(reg)
#define write_sysreg_s(value, reg) mock_write(value, reg)
#define isb() (++barriers)
'''

TESTS = r'''
static void reset(unsigned int mode, bool deep, unsigned int variant) {
    implementor = ARM_CPU_IMP_APPLE;
    part = APPLE_CPU_PART_TSMC_TWISTER;
    /* Exercise preservation of unrelated IRQ, retention and upper bits. */
    cfg = BIT_ULL(30) | (deep ? BIT_ULL(24) : 0);
    cyc = (UINT64_C(0x87654321f0abcdef) & ~(UINT64_C(3) << 24)) |
          ((u64)mode << 24);
    cyc ^= variant ? UINT64_C(0x1000000000f00001) : 0;
    original = cyc;
    reads = writes = waits = barriers = 0;
}
int main(void) {
    unsigned int cases = 0;
    for (unsigned int mode = 0; mode < 4; mode++) {
        for (unsigned int deep = 0; deep < 2; deep++) {
            for (unsigned int variant = 0; variant < 2; variant++) {
                reset(mode, deep, variant);
                int ret = a9_idle_wfi(NULL, NULL, 0);
                assert(ret == ((!deep && mode == 2) ? 0 : -EBUSY));
                assert(waits == (!deep && mode == 2));
                assert(!writes && cyc == original);
                cases++;

                reset(mode, deep, variant);
                ret = a9_idle_clock_gate(NULL, NULL, 1);
                if (!deep && mode == 0) {
                    assert(ret == 1 && waits == 1 && writes == 2 && barriers == 2);
                    assert(written[0] == ((original & ~A9_WFI_MODE) | A9_FORCE_CLOCK_GATE));
                    assert(written[1] == original);
                } else {
                    assert(ret == ((!deep && mode == 2) ? 0 : -EBUSY));
                    assert(waits == (!deep && mode == 2));
                    assert(!writes);
                }
                assert(cyc == original);
                cases++;
            }
        }
    }
    for (unsigned int kind = 0; kind < 2; kind++) {
        reset(2, false, 0);
        if (kind) implementor = 0x41;
        else part = 0x22;
        assert(a9_idle_wfi(NULL, NULL, 0) == -ENODEV);
        assert(a9_idle_clock_gate(NULL, NULL, 1) == -ENODEV);
        assert(!reads && !writes && !waits);
        cases += 2;
    }
    printf("%u entry-guard scenarios passed (mocked hardware; physical wake untested)\n", cases);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    source = args.source.read_text()
    # Include the original constants and entry functions, excluding Linux-only
    # registration/probe machinery. No implementation is copied into this test.
    body = source[source.index('#define A9_CPU_CFG'):source.index('static struct cpuidle_driver')]
    with tempfile.TemporaryDirectory(prefix='a9-idle-guards-') as directory:
        base = Path(directory)
        (base / 'guards.c').write_text(PRELUDE + body + TESTS)
        subprocess.run([args.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-variable',
                        '-O2', str(base / 'guards.c'), '-o', str(base / 'guards')], check=True)
        subprocess.run([str(base / 'guards')], check=True)


if __name__ == '__main__':
    main()
