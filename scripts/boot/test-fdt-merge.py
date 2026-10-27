#!/usr/bin/env python3
"""Offline semantic DTB merge tests, including the real round-5 old/new pair.

The running FDT is a fixture of documented m1n1 fixups, NOT a device capture.
"""
import copy
import importlib.machinery
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
F = importlib.machinery.SourceFileLoader('fdt', str(HERE/'device/linux-a9-fdt-merge')).load_module()
OLD = Path(sys.argv[1])
NEW = Path(sys.argv[2])
sys.argv = sys.argv[:1]


def cells(*values):
    return struct.pack(f'>{len(values)}I', *values)


def node(parent, name, **props):
    n = F.Node(name)
    n.props.update(props)
    parent.children[name] = n
    return n


def fixture():
    old = F.Node('')
    a = node(old, 'clock', phandle=cells(1))
    a.props['#clock-cells'] = cells(1)
    b = node(old, 'domain', phandle=cells(2))
    b.props['#power-domain-cells'] = cells(0)
    node(old, 'user', clocks=cells(1, 2))
    run, new = copy.deepcopy(old), copy.deepcopy(old)
    new.children['clock'].props['phandle'] = cells(2)
    new.children['domain'].props['phandle'] = cells(1)
    new.children['user'].props['clocks'] = cells(2, 2)
    return old, run, new


class MergeTests(unittest.TestCase):
    def test_real_pair_runtime_framebuffer_flash_psci_memory(self):
        old = F.load(OLD)[0]
        new, nrsv, _ = F.load(NEW)
        original = copy.deepcopy(new)
        run = copy.deepcopy(old)
        chosen = run.children['chosen']
        fbname = next(n for n in chosen.children if n.startswith('framebuffer'))
        fb = chosen.children.pop(fbname)
        fb.name = 'framebuffer@87c95c000'
        fb.props['reg'] = cells(8, 0x7c95c000, 0, 0x600000)
        chosen.children[fb.name] = fb
        node(run, 'psci', compatible=b'arm,psci-1.0\0', method=b'smc\0')
        for _, n in F.walk(run.children['cpus']):
            if n.props.get('device_type') == b'cpu\0':
                n.props['enable-method'] = b'psci\0'
        chosen.props['linux-a9,m1n1-reentry'] = cells(8, 0x5a00000)
        reserved = run.children['reserved-memory']
        # Allocate synthetic firmware IDs above both real trees. The target
        # can grow new providers (for example the D2365 event controller).
        high = max(F.phandle_of(n) or 0 for tree in (old, new)
                   for _, n in F.walk(tree))
        for i, addr in enumerate((0x804a00000, 0x87bbe0000), 1):
            n = node(reserved, f'flash@{addr:x}', compatible=b'phram\0', phandle=cells(high+i))
            n.props['reg'] = cells(addr >> 32, addr & 0xffffffff, 0, 0x4000)
        chosen.props['memory-region'] = cells(high+1, high+2)
        log = node(reserved, 'linux-a9-pstore', compatible=b'ramoops\0')
        log.props['reg'] = cells(8, 0x73000000, 0, 0x200000)
        log.props['no-map'] = b''
        # New nodes can reuse firmware-only IDs; references must stay by path.
        added = node(new, 'new-collision', phandle=cells(high+1))
        err, _, _ = F.merge(old, run, new)
        self.assertEqual(err, [])
        ix = F.index(new)
        self.assertNotIn('/chosen/'+fbname, ix)
        self.assertEqual(ix['/chosen/'+fb.name].props['reg'], fb.props['reg'])
        old_ids = {F.phandle_of(n): p for p,n in F.walk(old) if F.phandle_of(n)}
        expected = [F.phandle_of(ix[old_ids[v]]) for v in
                    struct.unpack('>II', fb.props['power-domains'])]
        self.assertEqual(ix['/chosen/'+fb.name].props['power-domains'], cells(*expected))
        self.assertEqual(ix['/psci'].props['method'], b'smc\0')
        refs = struct.unpack('>II', chosen.props['memory-region'])
        actual = struct.unpack('>II', ix['/chosen'].props['memory-region'])
        self.assertNotEqual(refs, actual)
        for ref, addr in zip(actual, (0x804a00000, 0x87bbe0000)):
            self.assertEqual(ref, F.phandle_of(ix[f'/reserved-memory/flash@{addr:x}']))
        for p, n in F.index(original).items():
            if p.startswith('/chosen/framebuffer'):
                continue
            if F.phandle_of(n):
                self.assertEqual(F.phandle_of(ix[p]), F.phandle_of(n), p)
        for path in set(F.index(original))-set(F.index(old)):
            self.assertIn(path, ix)
        self.assertEqual(ix['/reserved-memory/linux-a9-pstore'].props, log.props)
        # Validate serialized output with the independent dtc implementation.
        with tempfile.TemporaryDirectory() as d:
            f = Path(d)/'merged.dtb'
            f.write_bytes(F.serialize(new, nrsv, 0))
            subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dtb', '-o', '/dev/null', str(f)], check=True)
        print(f'real pair: {OLD} + {NEW}; {sum(F.phandle_of(n) != F.phandle_of(F.index(original).get(p, F.Node(""))) for p,n in F.walk(old))} old phandles differ')

    def test_identity_preserves_numbering_and_inputs(self):
        old = F.load(OLD)[0]
        before = F.serialize(old, [], 0)
        new = copy.deepcopy(old)
        self.assertEqual(F.merge(old, old, new)[0], [])
        self.assertEqual(F.serialize(new, [], 0), before)
        self.assertEqual(F.serialize(old, [], 0), before)

    def test_argument_equal_to_old_phandle_is_not_remapped(self):
        old, run, new = fixture()
        node(run, 'runtime', clocks=cells(1, 2))
        self.assertEqual(F.merge(old, run, new)[0], [])
        self.assertEqual(new.children['runtime'].props['clocks'], cells(2, 2))

    def test_nested_runtime_reference_remapped(self):
        old, run, new = fixture()
        n = node(run, 'runtime')
        node(n, 'child', **{'memory-region': cells(1)})
        self.assertEqual(F.merge(old, run, new)[0], [])
        self.assertEqual(new.children['runtime'].children['child'].props['memory-region'], cells(2))

    def test_runtime_reference_retarget_is_genuine_conflict(self):
        old, run, new = fixture()
        old.children['user'].props['memory-region'] = cells(1)
        run.children['user'].props['memory-region'] = cells(2)
        node(new, 'other', phandle=cells(3))
        new.children['user'].props['memory-region'] = cells(3)
        self.assertTrue(F.merge(old, run, new)[0])

    def test_pruned_node_renumber_only(self):
        old, run, new = fixture()
        del run.children['user']
        self.assertEqual(F.merge(old, run, new)[0], [])
        self.assertNotIn('user', new.children)

    def test_pruned_node_real_change_refused(self):
        old, run, new = fixture()
        del run.children['user']
        new.children['user'].props['status'] = b'disabled\0'
        self.assertTrue(F.merge(old, run, new)[0])

    def test_changed_runtime_node_removed_refused(self):
        old, run, new = fixture()
        run.children['user'].props['status'] = b'okay\0'
        del new.children['user']
        self.assertTrue(F.merge(old, run, new)[0])

    def test_duplicate_invalid_and_inconsistent_handles(self):
        for val in (cells(1), cells(0), cells(0xffffffff), b'bad'):
            old, run, new = fixture()
            node(run, 'duplicate', phandle=val)
            self.assertTrue(F.merge(old, run, new)[0])
        old, run, new = fixture()
        run.children['clock'].props['linux,phandle'] = cells(9)
        self.assertTrue(F.merge(old, run, new)[0])

    def test_dangling_and_truncated_references_refused(self):
        for name, value in [('clocks', cells(99, 1)), ('clocks', cells(1)), ('memory-region', b'bad')]:
            old, run, new = fixture()
            node(run, 'runtime', **{name: value})
            self.assertTrue(F.merge(old, run, new)[0])

    def test_removed_referenced_provider_refused(self):
        old, run, new = fixture()
        node(run, 'runtime', **{'memory-region': cells(2)})
        del new.children['domain']
        self.assertTrue(F.merge(old, run, new)[0])

    def test_variable_cells_maps_gpio_and_zero_slots(self):
        old, run, new = fixture()
        for tree in (old, run, new):
            tree.children['clock'].props['#gpio-cells'] = cells(2)
        n = node(run, 'runtime')
        n.props.update({'power-domains': cells(2), 'gpios': cells(1, 2, 1),
                        'iommu-map': cells(2, 1, 1, 2), 'gpio-ranges': cells(1, 1, 2, 3),
                        'assigned-clocks': cells(0, 1, 2)})
        self.assertEqual(F.merge(old, run, new)[0], [])
        actual = new.children['runtime'].props
        self.assertEqual(actual['gpios'], cells(2, 2, 1))
        self.assertEqual(actual['iommu-map'], cells(2, 2, 1, 2))
        self.assertEqual(actual['gpio-ranges'], cells(2, 1, 2, 3))
        self.assertEqual(actual['assigned-clocks'], cells(0, 2, 2))

    def test_unsupported_layout_refused(self):
        old, run, new = fixture()
        node(run, 'runtime', **{'interrupt-map': cells(1, 2)})
        self.assertTrue(F.merge(old, run, new)[0])

    def test_cli_reservations_and_stale_chosen(self):
        old, run, new = fixture()
        chosen = node(run, 'chosen', **{'linux,initrd-start': cells(0, 0x10000),
                                      'linux,initrd-end': cells(0, 0x20000),
                                      'kaslr-seed': cells(1, 2)})
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            for name, tree, rsv in [('old', old, [(0x30000, 0x4000)]),
                    ('run', run, [(0x30000, 0x4000), (0x10000, 0x10000), (0x40000, 0x200000)]),
                    ('new', new, [(0x30000, 0x4000), (0x90000, 0x4000)])]:
                (p/name).write_bytes(F.serialize(tree, rsv, 0))
            subprocess.run([sys.executable, '-B', str(HERE/'device/linux-a9-fdt-merge'),
                            '--old', str(p/'old'), '--running', str(p/'run'), '--new', str(p/'new'),
                            '-o', str(p/'out')], check=True)
            tree, rsv, _ = F.load(p/'out')
            self.assertEqual(rsv, [(0x30000, 0x4000), (0x40000, 0x200000), (0x90000, 0x4000)])
            self.assertEqual(tree.children['chosen'].props, {'linux,booted-from-kexec': b''})

if __name__ == '__main__':
    unittest.main(verbosity=2)
