#!/usr/bin/env python3
"""Check the arm64 j71t pstore mapping contract before building; no device I/O.

The function names deliberately identify the boot-owned early path. A change
to its structure needs review of this check and the guarded real-libfdt fixture
in test-recovery-offline.py, rather than silently losing the boot-phase check.
"""
import argparse
from pathlib import Path
import re


def body(text, name):
    match = re.search(r'\b' + name + r'\([^;{}]*\)\s*\{', text)
    assert match, f'missing {name}'
    return text[match.end():text.index('\n}', match.end())]


def check(source):
    init = (source/'arch/arm64/mm/init.c').read_text()
    setup = body((source/'arch/arm64/kernel/setup.c').read_text(), 'setup_arch')
    reserve = body(init, 'apple_j71t_reserve_pstore')
    # Virtual memblock allocators clear their allocation through phys_to_virt.
    # Neither that alias nor __pa(fixmap) is valid at this point in setup_arch.
    forbidden = r'\b(?:memblock_alloc\w*|phys_to_virt|__phys_to_virt|__va|__pa|early_init_dt_verify)\s*\('
    assert not re.search(forbidden, reserve), 'pre-paging pstore uses a linear-map allocation/access or publishes a temporary mapping'
    assert 'early_memremap(' in reserve and 'early_memunmap(' in reserve, 'FDT writes need a bounded temporary early mapping'
    publish = body(init, 'apple_j71t_publish_pstore')
    assert 'early_init_dt_verify(phys_to_virt(' in publish, 'publish the permanent linear alias with its physical address'
    assert setup.index('arm64_memblock_init();') < setup.index('paging_init();') < setup.index('apple_j71t_publish_pstore();') < setup.index('unflatten_device_tree();') < setup.index('bootmem_init();'), 'pstore reserve/map/publish/unflatten order changed'
    memblock = body(init, 'arm64_memblock_init')
    assert memblock.index('early_init_fdt_scan_reserved_mem();') < memblock.index('apple_j71t_reserve_pstore();'), 'reserve after existing firmware/kernel/initrd reservations'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    check(args.source)
    print('PASS early pstore mapping contract: physical reserve -> temporary map -> paging -> permanent FDT -> unflatten')
