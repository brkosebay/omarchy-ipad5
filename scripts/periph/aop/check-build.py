#!/usr/bin/env python3
"""Run in a built Docker AOP/integration tree; verify DT isolation, gates and symbols."""
from pathlib import Path
import argparse
import re
import runpy
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=('off', 'on'))
    ap.add_argument('--tree', default=Path.cwd().name, help='built Docker tree (aop or integration tree)')
    ap.add_argument('--integration-base', help='simulate a merge, without changing any branch/ref')
    args = ap.parse_args()
    config = Path('.config').read_text().splitlines()
    enabled = args.mode == 'on'
    for key in ('APPLE_A9_AOP', 'APPLE_A9_AOP_IIO', 'APPLE_A9_AOP_ACTIVE'):
        line = f'CONFIG_{key}=y' if enabled else f'# CONFIG_{key} is not set'
        # Invisible dependent symbol may be omitted when its parent is off.
        assert line in config or (not enabled and not any(s.startswith(f'CONFIG_{key}=') for s in config))
    assert 'CONFIG_DEBUG_INFO_NONE=y' in config
    assert re.fullmatch(r'[A-Za-z0-9_-]+', args.tree), args.tree
    out = Path(f'/work/build/kernel-{args.tree}')
    assert (out / 'config').read_text() == Path('.config').read_text()
    dtb = out / 'dtbs/s8003-j71t.dtb'
    node = '/soc/aop-observer@210800044'
    active_node = '/soc/aop@210800000'

    def prop(name, kind='x'):
        return subprocess.check_output(['fdtget', '-t', kind, str(dtb), node, name], text=True).split()

    cells = [int(n, 16) for n in prop('reg')]
    assert len(cells) == 20
    addresses = [(cells[i] << 32) | cells[i + 1] for i in range(0, len(cells), 4)]
    assert addresses == [0x210800044, 0x210800b88, 0x210804008, 0x210804020, 0x210804040]
    assert all(cells[i + 2:i + 4] == [0, 4] for i in range(0, 20, 4))
    assert prop('compatible', 's') == ['apple,j71t-aop-observer']
    assert len(prop('apple,observe-domains')) == 5
    properties = subprocess.check_output(['fdtget', '-p', str(dtb), node], text=True).splitlines()
    assert not set(properties) & {'power-domains', 'resets', 'interrupts', 'memory-region', 'iommus'}
    for board in ('s8000-j71s', 's8000-j72s', 's8003-j72t'):
        for other in (node, active_node):
            result = subprocess.run(['fdtget', str(out / f'dtbs/{board}.dtb'), other, 'compatible'], capture_output=True)
            assert result.returncode != 0, f'wrong board opted in: {board} {other}'

    def aprop(name, kind='x'):
        return subprocess.check_output(['fdtget', '-t', kind, str(dtb), active_node, name], text=True).split()

    cells = [int(n, 16) for n in aprop('reg')]
    assert cells == [0x2, 0x10800000, 0, 0x1c000, 0x2, 0x10e00000, 0, 0x9b000,
                     0x2, 0x10000600, 0, 0x4], cells
    assert aprop('compatible', 's') == ['apple,j71t-aop']
    assert [int(n, 16) for n in aprop('interrupts')] == [0, 133, 4, 0, 130, 4]
    assert [int(n, 16) for n in aprop('apple,aop-target')] == [8]
    assert [int(n, 16) for n in aprop('apple,chip-id')] == [0x8003]
    assert [int(n, 16) for n in aprop('apple,chip-revision')] == [1]
    assert len(aprop('apple,observe-domains')) == 5
    properties = subprocess.check_output(['fdtget', '-p', str(dtb), active_node], text=True).splitlines()
    assert not set(properties) & {'power-domains', 'resets', 'iommus', 'mount-matrix', 'status'}
    symbols = subprocess.check_output(['llvm-nm', '--defined-only', 'vmlinux'], text=True)
    for symbol in ('apple_pmgr_observe', 'a9_aop_probe', 'a9_aop_accel_register', 'a9_aop_accel_report',
                   'apple_a9_aop_debug', 'apple_a9_aop_attach', 'a9_aop_boot_work', 'a9_aop_irq', 'a9_aop_shutdown'):
        present = bool(re.search(rf'\b{symbol}$', symbols, re.M))
        assert present == enabled, (symbol, enabled)
    observer = Path('drivers/soc/apple/apple-a9-aop.c').read_text()
    pmgr = Path('drivers/pmdomain/apple/pmgr-pwrstate.c').read_text()
    observe_api = pmgr.split('/* A9 AOP observer API: BEGIN */')[1].split('/* A9 AOP observer API: END */')[0]
    forbidden = r'\b(?:writel\w*|writeq\w*|regmap_write|regmap_update_bits|request_irq|devm_request_irq|readq|ioread64|pm_runtime_get\w*|dev_pm_domain_attach\w*|dma_alloc\w*)\s*\('
    assert not re.search(forbidden, observer + observe_api)
    assert 'module_param(debug, bool, 0400)' in observer
    assert 'module_param(attach, bool, 0400)' in observer
    assert 'if ((!debug && !state_probe && !attach_stage) ||' in observer
    assert 'module_param(state_probe, bool, 0400)' in observer
    assert 'debugfs_create_file("status", 0400' in observer
    assert 'debugfs_create_file("preflight", 0400' in observer
    assert 'blocked-mailbox-fault' in observer
    assert observer.count('readl(') == 4
    state_read = observer.split('static void a9_aop_read_state(', 1)[1].split('\n}\n', 1)[0]
    assert state_read.count('readl(') == 2
    assert 'readl(snapshot->aop->regs[1])' in state_read
    assert 'readl(snapshot->aop->regs[3])' in state_read
    state_capture = observer.split('static int a9_aop_capture_state(', 1)[1].split('\n}\n', 1)[0]
    assert 'apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,' in state_capture
    assert 'a9_aop_read_state, &snapshot)' in state_capture
    assert 'states[1].raw' in state_capture and 'ret == -EPROBE_DEFER' in state_capture
    state_show = observer.split('static int a9_aop_state_show(', 1)[1].split('\n}\n', 1)[0]
    assert 'seq_puts(s, aop->state_line)' in state_show
    assert not re.search(r'\b(?:readl|apple_pmgr_observe|a9_aop_capture_state)\s*\(', state_show)
    assert 'if (apple_a9_aop_ring_mode() && !state_probe)' in observer
    assert 'debugfs_create_file("state_probe", 0400' in observer
    assert 'module_param(attach_stage, uint, 0400)' in observer
    assert 'debugfs_create_file("attach_status", 0400' in observer
    staged = observer.split('static int a9_aop_capture_attach(', 1)[1].split('DEFINE_SHOW_ATTRIBUTE(a9_aop_attach_status)', 1)[0]
    assert staged.index('if (attach_stage > 1)') < staged.index('apple_pmgr_observe(')
    assert 'delegated-to-active-experiment' in staged
    show = staged.split('static int a9_aop_attach_status_show(', 1)[1]
    assert 'seq_puts(s, aop->attach_line)' in show
    assert not re.search(r'\b(?:readl|apple_pmgr_observe|a9_aop_capture_attach)\s*\(', show)
    probe = observer.split('static int a9_aop_probe(', 1)[1]
    assert probe.index('if (attach_stage > 3)') < probe.index('devm_kzalloc(')
    assert probe.index('if (attach_stage > 1)') < probe.index('devm_ioremap_resource(')
    assert 'attach_stage && (debug || state_probe || attach || apple_a9_aop_ring_mode())' in probe
    assert not re.search(r'0x40(?:10|38)|0x400[04]', observer)
    reader = Path('drivers/soc/apple/a9-aop-ring-reader.c').read_text()
    assert not re.search(forbidden, reader)
    assert not re.search(r'\b(?:readl\w*|readq\w*|memcpy_fromio|memcpy_toio|'
                         r'memblock_reserve\w*|dma_sync\w*|set_memory\w*)\s*\(', reader)
    assert 'copy_from_kernel_nofault(' in reader and 'region_intersects(' in reader
    assert 'memblock_is_region_reserved(pa, PAGE_SIZE)' in reader
    assert 'PageReserved(pfn_to_page(PHYS_PFN(pa)))' in reader
    assert 'postcore_initcall_sync(rr_early_init)' in reader
    assert 'scan_reserved = apple_a9_aop_ring_scan_reserved()' in reader
    assert 'IORES_DESC_CRASH_KERNEL' in reader and 'cma_intersects(' in reader
    assert 'a9_rr_copy_pages(' in reader and 'A9_RR_SKIP_FAULT' in reader
    assert 'pfn_valid(pfn)' in reader and 'pfn_is_map_memory(pfn)' in reader
    assert '__ioremap_prot(rmem->base, rmem->size, PAGE_KERNEL_RO)' in reader
    assert 'historical_allocation=unknown' in reader
    assert 'module_param(ring_scan, bool, 0400)' in observer
    assert 'module_param(ring_scan_early, bool, 0400)' in observer
    assert 'module_param(ring_scan_reserved, bool, 0400)' in observer
    assert 'module_param_array(ring_pa, ullong, &ring_pa_nr, 0400)' in observer
    assert 'module_param_array(ring_pa_size, ullong, &ring_pa_size_nr, 0400)' in observer
    assert 'module_param(ring_iio, bool, 0400)' in observer
    target = Path('drivers/soc/apple/a9-aop-ring-target.c').read_text()
    assert not re.search(forbidden, target)
    assert not re.search(r'\b(?:readl\w*|readq\w*|ioremap\w*|__ioremap_prot|'
                         r'memcpy_fromio|memcpy_toio|memblock_reserve\w*|'
                         r'dma_sync\w*|set_memory\w*|k[vmz]alloc\w*)\s*\(', target)
    for guard in ('memblock_is_region_memory(pa, PAGE_SIZE)', 'region_intersects(',
                  'pfn_is_map_memory(pfn)', 'pfn_valid(pfn)', 'virt_addr_valid(phys_to_virt(pa))',
                  'copy_from_kernel_nofault(dst, phys_to_virt(pa), size)',
                  'a9_rr_copy_pages(', 'msleep(1000)', 'A9_RT_NOMAP', 'rt_bad_page('):
        assert guard in target, guard
    formatter = target.split('static void rt_dump(', 1)[1]
    assert not re.search(r'\b(?:phys_to_virt|rt_take|rt_capture_ranges|copy_from_kernel_nofault)\s*\(', formatter)
    assert 'debugfs_create_file("ring_pa", 0400' in target
    early = reader.split('static int __init rr_early_init(', 1)[1].split('postcore_initcall_sync', 1)[0]
    assert early.index('if (apple_a9_aop_attach_stage())') < early.index('a9_aop_ring_pa_capture();')
    assert early.index('a9_aop_ring_pa_capture();') < early.index('apple_a9_aop_ring_scan_early()')
    assert early.index('a9_aop_ring_pa_capture();') < early.index('rr_alloc(')
    assert observer.index('goto debugfs;') < observer.index('devm_ioremap_resource(')
    vmlinux = Path('vmlinux').read_bytes()
    assert (b'apple_a9_aop.active\0' in vmlinux) == enabled
    assert (b'apple_a9_aop.state_probe\0' in vmlinux) == enabled
    assert (b'apple_a9_aop.attach_stage\0' in vmlinux) == enabled
    assert (b'apple_a9_aop.ring_scan_reserved\0' in vmlinux) == enabled
    assert (b'apple_a9_aop.ring_pa\0' in vmlinux) == enabled
    assert (b'apple_a9_aop.ring_pa_size\0' in vmlinux) == enabled
    assert bool(re.search(r'\b__initcall_\w*rr_early_init2s$', symbols, re.M)) == enabled
    active = Path('drivers/soc/apple/apple-a9-aop-active.c').read_text()
    assert 'module_param_named(aop_force_cold, force_cold, bool, 0400)' in active
    assert (b'aop_force_cold\0' in vmlinux) == enabled
    assert (b'predicate=run-clear-delay' in vmlinux) == enabled
    assert b'bound_us=100000 reset_hook=none' not in vmlinux
    assert 'module_param(active, bool, 0400)' in active
    assert '#define MODULE_PARAM_PREFIX "apple_a9_aop."' in active
    assert 'if ((!active && stage < 2) || !of_machine_is_compatible("apple,j71t"))' in active
    probe = active.split('static int a9_aop_probe(')[1]
    assert probe.index('apple_a9_aop_ring_mode()') < probe.index('devm_kzalloc(')
    assert probe.index('apple_a9_aop_state_probe()') < probe.index('devm_kzalloc(')
    assert probe.index('apple_a9_aop_attach_stage()') < probe.index('devm_kzalloc(')
    assert probe.index('if ((!active') < probe.index('devm_')
    assert 'stage == 1 || stage > 3' in probe
    assert 'if (!stage) {' in probe and 'if (stage == 3)' in probe
    assert active.count('atomic_cmpxchg(&experiment_attempted, 0, 1)') == 2
    assert 'failed-force-halt' in active and 'failed-force-drain' in active
    stop = active.split('static int a9_aop_force_stop(', 1)[1].split('/* No invented FIFO', 1)[0]
    assert 'msleep(AOP_HALT_DELAY_MS)' in stop and 'readl_poll_timeout' not in stop
    assert 'predicate=run-clear-delay' in stop and 'execution=unknown' in stop
    predicate = active.split('static bool a9_aop_force_stopped(', 1)[1].split('\n}\n', 1)[0]
    assert 'AOP_WRAPPER_B88' not in predicate
    assert 'AOP_CPU_START' in predicate and 'AOP_OUTBOX_ENABLE | AOP_MBOX_FAULTS' in predicate
    assert 'cpu_status=' not in active + observer
    assert 'observed-mailbox-enabled-empty' in observer and 'observed-running-empty' not in observer
    assert 'module_param(wake, uint, 0400)' in active
    assert probe.index('wake > A9_EXP_WAKE_COMBINED') < probe.index('devm_kzalloc(')
    assert 'wake == A9_EXP_WAKE_AUTOBOOT || wake == A9_EXP_WAKE_COMBINED' in probe
    assert not re.search(r'0xc00[04]|0xc024', active, re.I), 'NMI block must remain untouched'
    assert (b'apple_a9_aop.wake\0' in vmlinux) == enabled
    assert 'devm_request_resource(aop->dev, parent, &aop->pool_resource)' in active
    assert 'MEMREMAP_WC' in active and 'a9_exp_alloc(' in active
    experiment = active.split('static bool a9_aop_experiment_handshake(', 1)[1].split('static void a9_aop_boot_work(', 1)[0]
    assert not re.search(r'\b(?:aop_wr|writel\w*|a9_aop_cold_start|enable_irq|dma_alloc\w*)\s*\(', experiment)
    assert 'stage2-handshake-complete' in experiment
    assert 'startup_transcript' in active and 'debugfs_create_file("samples"' in active
    pool = '/reserved-memory/aop-afk@858000000'
    assert subprocess.check_output(['fdtget', '-t', 'x', str(dtb), pool, 'reg'], text=True).split() == ['8', '58000000', '0', '400000']
    subprocess.run(['fdtget', str(dtb), pool, 'no-map'], check=True, stdout=subprocess.DEVNULL)
    assert aprop('memory-region') == subprocess.check_output(['fdtget', '-t', 'x', str(dtb), pool, 'phandle'], text=True).split()
    assert 'IRQF_NO_AUTOEN' in active
    assert 'refused-mailbox-fault' in active and 'refused-domain-state' in active
    assert 'a9_mgmt_ping_ack(payload, aop->ping_seq)' in active
    assert 'PHASE_ATTACH_PREFLIGHT' in active and 'PHASE_ATTACH_PING' in active and 'PHASE_ATTACH_HELLO' in active
    assert 'a9_attach_hello(&aop->takeover, apple_a9_aop_attach())' in active
    assert 'PageReserved(pfn_to_page(pfn))' in active
    assert 'of_reserved_mem_lookup(node)' in active and '"reusable"' in active
    assert 'sha256(fw->data, fw->size, digest)' in active
    assert not re.search(r'\bmemcpy_(?:from|to)io\s*\(', active)
    assert 'a9_aop_fromio32' in active and 'return readl(io->base + offset)' in active
    # The sole word-copy writer is reached only by the existing cold loader.
    assert active.count('a9_aop_copy_to32(') == 1
    copy_writer = active.split('static int a9_aop_copy_verify(', 1)[1].split('static int a9_aop_cold_start(', 1)[0]
    assert 'a9_aop_copy_to32(' in copy_writer
    assert active.count('a9_aop_copy_verify(') == 2
    attach = active.split('/* ---- running attach:', 1)[1].split('/* ---- bring-up', 1)[0]
    assert not re.search(r'\b(?:aop_wr|writel\w*|writeq\w*|memcpy_toio|a9_aop_cold_start|enable_irq|dma_alloc\w*|memblock_reserve)\s*\(', attach)
    preflight = attach.split('static bool a9_aop_attach_preflight(', 1)[1].split('static void a9_aop_attach_poll(', 1)[0]
    assert not re.search(r'\b(?:a9_aop_send|a9_aop_drain|a9_aop_inline_rx)\s*\(', preflight)
    assert preflight.index('if (!aop->diagnostics.mapping_matches)') < preflight.index('result = a9_attach_fingerprint(')
    snapshot = active.split('static void a9_aop_read_regs(', 1)[1].split('static int a9_aop_wait_firmware(', 1)[0]
    assert snapshot.index('diagnostics.remap[i]') < snapshot.index('a9_aop_fromio32(')
    assert snapshot.index('diagnostics.remap_after[i]') > snapshot.index('fp->table,')
    assert 'a9_attach_mapping_matches(' in snapshot
    policy = Path('drivers/soc/apple/a9-aop-takeover.h').read_text()
    assert 'refused-running-no-pending-hello' in policy
    assert 'A9_ATTACH_BUILD_ID_SIZE\t50' in policy
    assert 'live->build + A9_ATTACH_GUARD_OFFSET' in policy
    for refusal in ('foreign-image', 'no-loader-tags', 'tag-mismatch', 'stale-ext',
                    'unreserved-ext', 'unexpected-mgmt', 'no-ping-ack', 'handshake-owned'):
        assert f'refused-running-{refusal}' in policy
    assert 'debugfs_create_dir("apple-a9-aop-active"' in active
    assert 'debugfs_create_file("fingerprint", 0400' in active
    diagnostics = active.split('/* All diagnostic readers below', 1)[1].split('static int a9_aop_patchbay_show(', 1)[0]
    assert not re.search(r'\b(?:aop_rd|aop_wr|readl\w*|readq\w*|writel\w*|writeq\w*|memcpy_fromio|ioremap\w*|a9_aop_send|a9_aop_drain)\s*\(', diagnostics)
    assert 'a9_diag_next_tag' in diagnostics and 'actual_le32=' in diagnostics
    assert diagnostics.index('actual_le32=') < diagnostics.index('a9_diag_next_tag(')
    assert 'actual_printable=' in diagnostics and 'expected_printable=' in diagnostics
    # Never acknowledge/clear faults, never write the reset path.
    assert not re.search(r'function-device_reset|reset_control', active)
    assert not re.search(r'\b(?:aop_wr|writel|writeq)\s*\([^;]*\bAOP_MBOX_FAULTS\s*\)\s*;', active)
    templates = Path('/work/scripts/periph/aop/kernel')
    installer = runpy.run_path('/work/scripts/periph/aop/install-kernel.py')
    for name in installer['SOURCES']:
        assert (templates / name).read_bytes() == Path('drivers/soc/apple', name).read_bytes(), name
    for name in installer['BINDINGS']:
        assert (templates / name).read_bytes() == Path('Documentation/devicetree/bindings/soc/apple', name).read_bytes(), name
    for name in installer['DOCS']:
        assert (templates / name).read_bytes() == Path('Documentation/driver-api', name).read_bytes(), name
    for name in ('s8003-j71t-aop-observer.dtsi', 's8003-j71t-aop.dtsi'):
        assert (templates / name).read_bytes() == Path('arch/arm64/boot/dts/apple', name).read_bytes(), name
    assert Path('include/linux/soc/apple/pmgr-observe.h').read_bytes() == (templates / 'pmgr-observe.h').read_bytes()
    kconfig = Path('drivers/soc/apple/Kconfig').read_text()
    for key in ('APPLE_A9_AOP', 'APPLE_A9_AOP_IIO', 'APPLE_A9_AOP_ACTIVE'):
        body = kconfig.split(f'config {key}\n', 1)[1].split('\nconfig ', 1)[0]
        assert '\tdefault n\n' in body
    if args.integration_base:
        result = subprocess.run(['git', 'merge-tree', '--write-tree', '--name-only',
                                 args.integration_base, 'HEAD'], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f'merge simulation failed:\n{result.stdout}\n{result.stderr}')
        print(f'PASS: merge simulation onto {args.integration_base}; no branch/ref changed')
    print(f'PASS {args.mode}: observer five 32-bit resources and no write/FIFO/IRQ/DMA paths; J71t-only active node (wrapper/region/auto-boot, AIC 133/130, no power consumer); immutable active gate; templates installed; expected linked symbols')


if __name__ == '__main__':
    main()
