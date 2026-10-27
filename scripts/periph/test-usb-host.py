#!/usr/bin/env python3
"""Exercise mount-safe return and USB-only read/write safeguards without a device."""
import hashlib
import importlib.machinery
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
FILES = ROOT / 'scripts/omarchy/rootfs/files'
H = importlib.machinery.SourceFileLoader('host_trial', str(FILES / 'usr/local/sbin/linux-a9-usb-host')).load_module()
C = importlib.machinery.SourceFileLoader('usb_check', str(FILES / 'usr/local/bin/ipad-usb-check')).load_module()
D = importlib.machinery.SourceFileLoader(
    'iap_roleswitch', str(FILES / 'usr/local/sbin/linux-a9-iap-roleswitch')).load_module()


class FakeEp0:
    def __init__(self, payload=b''):
        self.payload = payload
        self.actions = []

    def reply(self, setup, data):
        self.actions.append(('reply', setup, data))

    def receive(self, setup):
        self.actions.append(('receive', setup))
        return self.payload

    def stall(self, setup):
        self.actions.append(('stall', setup))


class IapDescriptorTests(unittest.TestCase):
    def test_kernelcache_descriptor_bytes_and_lengths(self):
        self.assertEqual(len(D.REPORT_DESCRIPTOR_FS), 0x60)
        self.assertEqual(len(D.REPORT_DESCRIPTOR_OTHER), 0xd0)
        self.assertEqual(D.HID_DESCRIPTOR_FS, bytes.fromhex('09 21 11 01 00 01 22 60 00'))
        self.assertEqual(D.HID_DESCRIPTOR_OTHER,
                         bytes.fromhex('09 21 11 01 00 01 22 d0 00'))
        self.assertEqual(D.REPORT_DESCRIPTOR_FS[:14],
                         bytes.fromhex('06 00 ff 09 01 a1 01 75 08 26 80 00 15 00'))
        self.assertEqual(D.REPORT_DESCRIPTOR_FS[-8:],
                         bytes.fromhex('09 01 85 09 95 3f 92 02 01 c0')[-8:])
        self.assertEqual(D.REPORT_DESCRIPTOR_OTHER[-10:],
                         bytes.fromhex('09 01 85 15 95 ff 92 02 01 c0'))
        self.assertEqual(hashlib.sha256(D.REPORT_DESCRIPTOR_FS).hexdigest(),
                         'dc515ac99b69cae40074ddf55783155e32a09d7fbd9e9087d47fd3592f8cb288')
        self.assertEqual(hashlib.sha256(D.REPORT_DESCRIPTOR_OTHER).hexdigest(),
                         '900fc1696e0d575469facc1b83008e711121c965d477f9a4839364f6ce680103')

    def test_functionfs_blob_has_all_recipient_and_exact_interfaces(self):
        blob = D.descriptor_blob()
        magic, length, flags, fs_count, hs_count = struct.unpack_from('<IIIII', blob)
        self.assertEqual((magic, length, fs_count, hs_count), (3, len(blob), 3, 3))
        self.assertEqual(flags, D.FUNCTIONFS_HAS_FS_DESC |
                         D.FUNCTIONFS_HAS_HS_DESC | D.FUNCTIONFS_ALL_CTRL_RECIP)
        fs = D.descriptor_set(D.HID_DESCRIPTOR_FS)
        hs = D.descriptor_set(D.HID_DESCRIPTOR_OTHER)
        self.assertEqual(blob[20:], fs + hs)
        self.assertEqual(fs[:9], bytes.fromhex('09 04 00 00 01 03 00 00 00'))
        self.assertEqual(fs[-7:], bytes.fromhex('07 05 81 03 40 00 01'))
        self.assertEqual(D.strings_blob(), struct.pack('<IIII', 2, 16, 0, 0))


class IapResponderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / 'control.log'

    def responder(self, **kwargs):
        return D.Responder(self.log, **kwargs)

    def test_fake_event_stream_acks_then_waits_50ms_then_switches(self):
        order = []

        class OrderedEp0(FakeEp0):
            def receive(inner, setup):
                order.append('ack')
                return super().receive(setup)

        ep0 = OrderedEp0()
        responder = self.responder(sleep=lambda seconds: order.append(('sleep', seconds)),
                                   role_switch=lambda: order.append('switch'))
        event = D.event_bytes(D.FUNCTIONFS_SETUP, 0x40, 0x51, 0, 0, 0)
        self.assertEqual(responder.handle_event(event, ep0), 'role-switch')
        self.assertEqual(order, ['ack', ('sleep', 0.05), 'switch'])
        self.assertIn('bmRequestType=0x40 bRequest=0x51', self.log.read_text())

    def test_51_uses_subset_masks_and_rejects_outside_bits(self):
        responder = self.responder(value_mask=0x0005, index_mask=0x0020, sleep=lambda _s: None)
        accepted = FakeEp0()
        setup = (0x40, 0x51, 0x0001, 0x0020, 0)
        self.assertEqual(responder.handle_setup(setup, accepted), 'role-switch')
        rejected = FakeEp0()
        setup = (0x40, 0x51, 0x0002, 0x0020, 0)
        self.assertEqual(responder.handle_setup(setup, rejected), 'stall')
        self.assertEqual(rejected.actions[0][0], 'stall')

    def test_53_is_four_bytes_value_mask_then_index_mask(self):
        ep0 = FakeEp0()
        responder = self.responder(value_mask=0x1234, index_mask=0xabcd)
        setup = (0xc0, 0x53, 0, 0, 4)
        self.assertEqual(responder.handle_setup(setup, ep0), 'capabilities')
        self.assertEqual(ep0.actions, [('reply', setup, bytes.fromhex('34 12 cd ab'))])
        for bad in ((0xc0, 0x53, 0, 0, 2), (0xc0, 0x53, 1, 0, 4)):
            endpoint = FakeEp0()
            self.assertEqual(responder.handle_setup(bad, endpoint), 'stall')

    def test_hid_descriptor_reads_follow_link_speed(self):
        for speed, expected in [('full-speed', D.REPORT_DESCRIPTOR_FS),
                                ('high-speed', D.REPORT_DESCRIPTOR_OTHER)]:
            ep0 = FakeEp0()
            responder = self.responder(speed=lambda speed=speed: speed)
            setup = (0x81, 0x06, 0x2200, 0, len(expected))
            self.assertEqual(responder.handle_setup(setup, ep0), 'report-descriptor')
            self.assertEqual(ep0.actions[0][2], expected)

    def test_set_report_payload_is_consumed_and_logged(self):
        ep0 = FakeEp0(b'\x05\x00iap')
        responder = self.responder()
        setup = (0x21, 0x09, 0x0205, 0, 5)
        self.assertEqual(responder.handle_setup(setup, ep0), 'set-report')
        self.assertIn('SET_REPORT payload=05 00 69 61 70', self.log.read_text())

    def test_unknown_request_stalls_and_is_logged(self):
        ep0 = FakeEp0()
        setup = (0x21, 0x0a, 0, 0, 0)
        self.assertEqual(self.responder().handle_setup(setup, ep0), 'stall')
        self.assertIn('STALL', self.log.read_text())


class IapLifecycleTests(unittest.TestCase):
    def test_empty_udc_recovers_the_only_controller(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            gadget = base / 'g1'
            udcs = base / 'udcs'
            gadget.mkdir()
            udcs.mkdir()
            (gadget / 'UDC').write_text('\n')
            (udcs / '20c100000.usbdev').mkdir()
            self.assertEqual(D.discover_udc(gadget, udcs), '20c100000.usbdev')

    def test_ambiguous_empty_udc_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            gadget = base / 'g1'
            udcs = base / 'udcs'
            gadget.mkdir()
            udcs.mkdir()
            (gadget / 'UDC').write_text('\n')
            (udcs / 'one').mkdir()
            (udcs / 'two').mkdir()
            with self.assertRaisesRegex(RuntimeError, 'expected one available UDC'):
                D.discover_udc(gadget, udcs)

    def test_soft_reconnect_unbinds_waits_then_rebinds(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            gadget = base / 'g1'
            udcs = base / 'udcs'
            gadget.mkdir()
            udcs.mkdir()
            (gadget / 'UDC').write_text('20c100000.usbdev\n')
            (udcs / '20c100000.usbdev').mkdir()
            writes = []
            sleeps = []

            def write(value):
                writes.append(value)
                (gadget / 'UDC').write_text(value + '\n')

            D.soft_reconnect(gadget, udcs, sleep=sleeps.append, write_udc=write)
            self.assertEqual(writes, ['', '20c100000.usbdev'])
            self.assertEqual(sleeps, [0.100])

    def test_cleanup_removes_stale_iap_and_rebinds_c1(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            gadget = base / 'g1'
            config = gadget / 'configs/c.2'
            function = gadget / 'functions/ffs.iap'
            link = config / 'iap'
            mount = base / 'mount'
            role = base / 'role'
            udcs = base / 'udcs'
            config.mkdir(parents=True)
            function.mkdir(parents=True)
            mount.mkdir()
            udcs.mkdir()
            (udcs / '20c100000.usbdev').mkdir()
            (gadget / 'UDC').write_text('\n')
            role.write_text('device\n')
            link.symlink_to(function)
            ffs = D.FunctionFS(gadget, config, function, link, mount, role, udcs)
            ffs.cleanup(rebind=True)
            self.assertEqual((gadget / 'UDC').read_text().strip(),
                             '20c100000.usbdev')
            self.assertFalse(config.exists())
            self.assertFalse(function.exists())
            self.assertFalse(mount.exists())


class ReturnTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.role = Path(self.temp.name) / 'role'
        self.role.write_text('host\n')
        self.control = Path(self.temp.name) / 'control'
        self.control.mkdir()
        self.role_patch = mock.patch.object(H, 'ROLE', self.role)
        self.control_patch = mock.patch.object(H, 'CONTROL', self.control)
        self.role_patch.start()
        self.control_patch.start()
        self.addCleanup(self.role_patch.stop)
        self.addCleanup(self.control_patch.stop)

    def test_unmount_finishes_before_gadget_rebind(self):
        order = []

        def command(*args, **kwargs):
            self.assertEqual(self.role.read_text().strip(), 'host')
            self.assertEqual(args[0:2], ('udisksctl', 'unmount'))
            order.append(args[-1])
            return 0, 'unmounted\n'
        with mock.patch.object(H, 'mounted_usb_devices', return_value=['/dev/sda1', '/dev/sdb1']), \
             mock.patch.object(H, 'command', side_effect=command), mock.patch.object(H.os, 'sync') as sync:
            H.return_device()
        self.assertEqual(order, ['/dev/sda1', '/dev/sdb1'])
        sync.assert_called_once()
        self.assertEqual(self.role.read_text().strip(), 'device')

    def test_busy_drive_keeps_host_and_cancels_kernel_revert(self):
        with mock.patch.object(H, 'mounted_usb_devices', return_value=['/dev/sda1']), \
             mock.patch.object(H, 'command', return_value=(1, 'busy\n')):
            with self.assertRaises(RuntimeError):
                H.return_device()
        self.assertEqual(self.role.read_text().strip(), 'host')
        self.assertEqual((self.control / 'confirm').read_text().strip(), '1')

    def test_unknown_mount_state_also_keeps_host(self):
        with mock.patch.object(H, 'mounted_usb_devices', side_effect=ValueError('bad lsblk JSON')):
            with self.assertRaises(ValueError):
                H.return_device()
        self.assertEqual(self.role.read_text().strip(), 'host')
        self.assertTrue((self.control / 'confirm').exists())

    def test_usb_bus_inherited_by_partition_but_nvme_excluded(self):
        tree = {'blockdevices': [
            {'name': '/dev/nvme0n1', 'tran': 'nvme', 'mountpoints': [None], 'children': [
                {'name': '/dev/nvme0n1p2', 'tran': None, 'mountpoints': ['/']}]},
            {'name': '/dev/sda', 'tran': 'usb', 'mountpoints': [None], 'children': [
                {'name': '/dev/sda1', 'tran': None, 'mountpoints': ['/run/media/linux/TEST']},
                {'name': '/dev/sda2', 'tran': None, 'mountpoints': [None]}]}]}
        with mock.patch.object(H, 'command', return_value=(0, json.dumps(tree))):
            self.assertEqual(H.mounted_usb_devices(), ['/dev/sda1'])

    def test_failed_lsblk_cannot_look_like_no_mounts(self):
        with mock.patch.object(H, 'command', return_value=(1, 'failed')):
            with self.assertRaises(RuntimeError):
                H.mounted_usb_devices()

    def test_trial_still_records_logs_when_cleanup_refuses_busy_disk(self):
        self.role.write_text('device\n')
        runtime = Path(self.temp.name) / 'runtime'
        logs = Path(self.temp.name) / 'logs'
        runtime.mkdir()
        logs.mkdir()
        labels = []
        with mock.patch.object(H, 'RUNTIME', runtime), mock.patch.object(H, 'LOGS', logs), \
             mock.patch.object(H, 'preflight'), mock.patch.object(H, 'snapshot',
                 side_effect=lambda _dest, label: labels.append(label)), \
             mock.patch.object(H, 'command', return_value=(0, 'saved journal\n')), \
             mock.patch.object(H.time, 'monotonic', side_effect=[0, 31]), \
             mock.patch.object(H.time, 'sleep'), mock.patch.object(H.os, 'sync'), \
             mock.patch.object(H, 'return_device', side_effect=RuntimeError('busy')):
            with self.assertRaisesRegex(RuntimeError, 'busy'):
                H.trial('none', 5, 30)
        self.assertEqual(labels, ['before', 'after'])
        dest = next(p for p in logs.iterdir() if p.is_dir())
        self.assertEqual((dest / 'kernel-journal.txt').read_text(), 'saved journal\n')
        self.assertTrue((dest / 'udisks-journal.txt').is_file())


class IdentifyTrialTests(unittest.TestCase):
    """--identify: GetID after the cable-swap delay, right before host mode."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.role = base / 'role'
        self.role.write_text('device\n')
        self.logs = base / 'logs'
        self.idbus = base / 'apple-tristar'
        self.ph = base / 'allow_power_handshake'
        self.ph.write_text('N\n')
        for name in ('control', 'runtime', 'logs', 'apple-tristar'):
            (base / name).mkdir()
        for name, text in [('identify', ''), ('power_handshake', ''),
                           ('digital_id', '10 0c 00 00 00 00'),
                           ('digital_id_decoded', 'USB=2 ACCx=0 DB=0 MB=0 UART=0 PWR=0 HV=3'),
                           ('idbus_frame', 'frame: 75 10 0c 00 00 00 00 66 (header and CRC ok)'),
                           ('reg_05', '0x00'), ('reg_0d', '0x10'), ('reg_0a', '0x00'),
                           ('identify_log', 'baseline 0x0c = 0x83'),
                           ('power_handshake_log', 'power handshake ok (0)')]:
            (self.idbus / name).write_text(text + '\n')
        for name, value in [('ROLE', self.role), ('CONTROL', base / 'control'),
                            ('RUNTIME', base / 'runtime'), ('LOGS', self.logs),
                            ('IDBUS', self.idbus), ('PH_PARAM', self.ph)]:
            patcher = mock.patch.object(H, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        for name in ('preflight', 'snapshot'):
            patcher = mock.patch.object(H, name)
            patcher.start()
            self.addCleanup(patcher.stop)
        for patcher in (mock.patch.object(H, 'command', return_value=(0, '')),
                        mock.patch.object(H.os, 'sync')):
            patcher.start()
            self.addCleanup(patcher.stop)

    def run_trial(self, result, *args):
        events = []

        def trigger(path, text):
            events.append(('trigger', path.name, text, self.role.read_text().strip(),
                           self.ph.read_text().strip()))
            return result
        with mock.patch.object(H.time, 'sleep', side_effect=lambda s: events.append(('sleep', s))), \
             mock.patch.object(H.time, 'monotonic', side_effect=[0, 181]), \
             mock.patch.object(H, 'trigger', side_effect=trigger), \
             mock.patch.object(H, 'return_device', side_effect=lambda: events.append(('device',))):
            H.trial('none', 20, 180, *args)
        return events

    def trial_dir(self):
        return next(p for p in self.logs.iterdir() if p.is_dir())

    def test_identify_after_delay_before_host(self):
        events = self.run_trial((0, 'ok'), 'default')
        self.assertEqual(events[0], ('sleep', 20))
        self.assertEqual(events[1], ('trigger', 'identify', 'identify', 'device', 'N'))
        self.assertEqual([e[0] for e in events].count('trigger'), 1)
        self.assertEqual(events[-1], ('device',))
        self.assertEqual(self.role.read_text().strip(), 'host')
        text = (self.trial_dir() / 'identify.txt').read_text()
        self.assertIn('digital_id: 10 0c 00 00 00 00', text)
        self.assertIn('baseline 0x0c = 0x83', text)
        self.assertEqual((self.logs / 'latest-identify.txt').read_text(), text)

    def test_failed_identify_skips_handshake_but_arms_host(self):
        events = self.run_trial((110, 'failed: Connection timed out'), 'default', True)
        self.assertEqual([e[1] for e in events if e[0] == 'trigger'], ['identify'])
        self.assertEqual(self.role.read_text().strip(), 'host')
        self.assertEqual(self.ph.read_text().strip(), 'N')
        self.assertIn('failed: Connection timed out',
                      (self.trial_dir() / 'identify.txt').read_text())

    def test_handshake_gate_open_only_during_its_trigger(self):
        events = self.run_trial((0, 'ok'), 'swap', True)
        triggers = [e for e in events if e[0] == 'trigger']
        self.assertEqual([(t[1], t[2], t[4]) for t in triggers],
                         [('identify', 'identify swap', 'N'), ('power_handshake', '1', 'Y')])
        self.assertTrue(all(t[3] == 'device' for t in triggers))
        self.assertEqual(self.ph.read_text().strip(), 'N')
        self.assertTrue((self.trial_dir() / 'power-handshake.txt').is_file())

    def test_handshake_gate_closed_even_if_trigger_raises(self):
        with mock.patch.object(H, 'trigger', side_effect=OSError('boom')):
            with self.assertRaises(OSError):
                H.power_handshake(self.logs)
        self.assertEqual(self.ph.read_text().strip(), 'N')

    def test_plain_trial_never_touches_idbus(self):
        events = self.run_trial((0, 'ok'))
        self.assertNotIn('trigger', [e[0] for e in events])
        self.assertFalse((self.logs / 'latest-identify.txt').exists())

    def test_missing_debugfs_refuses_before_delay(self):
        (self.idbus / 'identify').unlink()
        with mock.patch.object(H.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'usb-host3'):
                H.trial('none', 20, 180, 'default')
        sleep.assert_not_called()
        self.assertEqual(self.role.read_text().strip(), 'device')

    def test_arm_forwards_identify_to_detached_trial(self):
        calls = []

        def command(*args, **kwargs):
            calls.append(args)
            return (3, '') if args[0] == 'systemctl' else (0, '')
        argv = ['linux-a9-usb-host', 'arm', 'none', '20', '180', '--identify', '--power-handshake']
        with mock.patch.object(H, 'command', side_effect=command), \
             mock.patch.object(H.sys, 'argv', argv):
            H.main()
        run = next(c for c in calls if c[0] == 'systemd-run')
        self.assertEqual(run[-7:], (H.SELF, 'trial', 'none', '20', '180',
                                    '--identify=default', '--power-handshake'))

    def test_handshake_without_identify_is_rejected(self):
        argv = ['linux-a9-usb-host', 'arm', 'none', '20', '180', '--power-handshake']
        with mock.patch.object(H.sys, 'argv', argv), mock.patch('sys.stderr'):
            with self.assertRaises(SystemExit):
                H.main()


class ListenActionTests(unittest.TestCase):
    def test_listen_detaches_without_a_plug_in_countdown(self):
        calls = []

        def command(*args, **kwargs):
            calls.append(args)
            return (3, '') if args[0] == 'systemctl' else (0, 'started\n')

        with mock.patch.object(H, 'preflight'), mock.patch.object(H, 'preflight_idbus'), \
             mock.patch.object(H, 'command', side_effect=command), mock.patch('sys.stdout'):
            H.listen_main(['pmu', '240', '--identify'])
        run = next(call for call in calls if call[0] == 'systemd-run')
        self.assertEqual(run[-5:], (H.SELF, 'listen-service', 'pmu', '240',
                                    '--identify=default'))
        self.assertNotIn('delay', run)

    def test_listen_duration_is_host_window_not_a_short_deadline(self):
        with mock.patch.object(H, 'preflight'), mock.patch('sys.stderr'):
            with self.assertRaises(SystemExit):
                H.listen_main(['none', '29'])

    def test_device_stops_listener_before_mount_safe_role_return(self):
        calls = []

        def command(*args, **kwargs):
            calls.append(args)
            return 0, ''

        argv = ['linux-a9-usb-host', 'device']
        daemon = mock.Mock()
        with mock.patch.object(H, 'preflight'), mock.patch.object(H, 'command', side_effect=command), \
             mock.patch.object(H, 'return_device') as returned, \
             mock.patch.object(H, 'load_iap_daemon', return_value=daemon), \
             mock.patch.object(H.sys, 'argv', argv):
            H.main()
        self.assertIn(('systemctl', 'stop', 'linux-a9-usb-host-listen.service'), calls)
        returned.assert_called_once()
        daemon.repair_gadget.assert_called_once()

    def test_device_repairs_after_collected_listener_unit_disappears(self):
        calls = []

        def command(*args, **kwargs):
            calls.append(args)
            if args[:2] == ('systemctl', 'stop'):
                return 5, 'Unit linux-a9-usb-host-listen.service not loaded.\n'
            if args[:2] == ('systemctl', 'is-active'):
                return 3, 'inactive\n'
            return 0, ''

        argv = ['linux-a9-usb-host', 'device']
        daemon = mock.Mock()
        with mock.patch.object(H, 'preflight'), mock.patch.object(H, 'command', side_effect=command), \
             mock.patch.object(H, 'return_device') as returned, \
             mock.patch.object(H, 'load_iap_daemon', return_value=daemon), \
             mock.patch.object(H.sys, 'argv', argv), mock.patch('sys.stdout'), \
             mock.patch('sys.stderr'):
            H.main()
        returned.assert_called_once()
        daemon.repair_gadget.assert_called_once()

    def test_device_refuses_cleanup_if_listener_remains_active(self):
        def command(*args, **_kwargs):
            if args[:2] == ('systemctl', 'stop'):
                return 1, 'stop failed\n'
            if args[:2] == ('systemctl', 'is-active'):
                return 0, 'active\n'
            return 0, ''

        argv = ['linux-a9-usb-host', 'device']
        with mock.patch.object(H, 'preflight'), mock.patch.object(H, 'command', side_effect=command), \
             mock.patch.object(H, 'return_device') as returned, \
             mock.patch.object(H.sys, 'argv', argv), mock.patch('sys.stdout'):
            with self.assertRaisesRegex(RuntimeError, 'active iAP listener'):
                H.main()
        returned.assert_not_called()

    def test_listener_role_callback_uses_shared_host_window(self):
        class Daemon:
            @staticmethod
            def serve(_log, role_switch, **_kwargs):
                role_switch()
                raise KeyboardInterrupt

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            role = base / 'role'
            role.write_text('device\n')
            for name in ('runtime', 'logs'):
                (base / name).mkdir()
            patches = [mock.patch.object(H, 'ROLE', role),
                       mock.patch.object(H, 'RUNTIME', base / 'runtime'),
                       mock.patch.object(H, 'LOGS', base / 'logs'),
                       mock.patch.object(H, 'preflight'), mock.patch.object(H, 'snapshot'),
                       mock.patch.object(H, 'load_iap_daemon', return_value=Daemon),
                       mock.patch.object(H, 'host_window')]
            for patcher in patches:
                patcher.start()
                self.addCleanup(patcher.stop)
            with self.assertRaises(KeyboardInterrupt):
                H.listen_service('tristar', 180)
            H.host_window.assert_called_once()
            self.assertEqual(H.host_window.call_args.args[0:2], ('tristar', 180))

    def test_successful_adapter_handshake_reconnects_before_sideband_only(self):
        identified = threading.Event()
        handshake_done = threading.Event()
        order = []

        class Daemon:
            @staticmethod
            def soft_reconnect():
                order.append('reconnect')

            @staticmethod
            def serve(_log, role_switch, ready):
                ready()
                if not identified.wait(2):
                    raise AssertionError('attachment monitor did not run GetID')
                if not handshake_done.wait(2):
                    raise AssertionError('power handshake did not run')
                threading.Event().wait(0.2)
                raise KeyboardInterrupt

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            role = base / 'role'
            role.write_text('device\n')
            runtime = base / 'runtime'
            logs = base / 'logs'
            idbus = base / 'idbus'
            runtime.mkdir()
            logs.mkdir()
            idbus.mkdir()
            (idbus / 'digital_id').write_text(H.CAMERA_ADAPTER_ID + '\n')
            online = base / 'online'
            online.write_text('1\n')
            dest = logs / 'listen'
            dest.mkdir()
            with mock.patch.object(H, 'ROLE', role), mock.patch.object(H, 'RUNTIME', runtime), \
                 mock.patch.object(H, 'LOGS', logs), mock.patch.object(H, 'IDBUS', idbus), \
                 mock.patch.object(H, 'POWER_ONLINE', online), \
                 mock.patch.object(H, 'LISTEN_IDENTIFY_REQUEST', runtime / 'request'), \
                 mock.patch.object(H, 'LISTEN_DIR', runtime / 'listen-dir'), \
                 mock.patch.object(H, 'preflight'), mock.patch.object(H, 'preflight_idbus'), \
                 mock.patch.object(H, 'snapshot'), \
                 mock.patch.object(H, 'new_log_dir', return_value=dest), \
                 mock.patch.object(H, 'load_iap_daemon', return_value=Daemon), \
                 mock.patch.object(H, 'identify',
                                   side_effect=lambda *_a: order.append('identify') or
                                   identified.set() or True) as identify, \
                 mock.patch.object(H, 'power_handshake',
                                   side_effect=lambda *_a: order.append('handshake') or
                                   handshake_done.set() or True) as handshake:
                with self.assertRaises(KeyboardInterrupt):
                    H.listen_service('none', 180, 'default', True)
            self.assertTrue(identify.called)
            handshake.assert_called_once()
            self.assertEqual(order, ['reconnect', 'identify', 'handshake'])

    def test_failed_adapter_handshake_does_not_disconnect_after_reply(self):
        identified = threading.Event()
        handshake_done = threading.Event()
        reconnect_count = 0

        class Daemon:
            @staticmethod
            def soft_reconnect():
                nonlocal reconnect_count
                reconnect_count += 1

            @staticmethod
            def serve(_log, role_switch, ready):
                ready()
                if not identified.wait(2):
                    raise AssertionError('attachment monitor did not run GetID')
                if not handshake_done.wait(2):
                    raise AssertionError('power handshake did not run')
                threading.Event().wait(0.2)
                raise KeyboardInterrupt

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            role = base / 'role'
            role.write_text('device\n')
            runtime = base / 'runtime'
            logs = base / 'logs'
            idbus = base / 'idbus'
            runtime.mkdir()
            logs.mkdir()
            idbus.mkdir()
            (idbus / 'digital_id').write_text(H.CAMERA_ADAPTER_ID + '\n')
            online = base / 'online'
            online.write_text('1\n')
            dest = logs / 'listen'
            dest.mkdir()
            with mock.patch.object(H, 'ROLE', role), mock.patch.object(H, 'RUNTIME', runtime), \
                 mock.patch.object(H, 'LOGS', logs), mock.patch.object(H, 'IDBUS', idbus), \
                 mock.patch.object(H, 'POWER_ONLINE', online), \
                 mock.patch.object(H, 'LISTEN_IDENTIFY_REQUEST', runtime / 'request'), \
                 mock.patch.object(H, 'LISTEN_DIR', runtime / 'listen-dir'), \
                 mock.patch.object(H, 'preflight'), mock.patch.object(H, 'preflight_idbus'), \
                 mock.patch.object(H, 'snapshot'), \
                 mock.patch.object(H, 'new_log_dir', return_value=dest), \
                 mock.patch.object(H, 'load_iap_daemon', return_value=Daemon), \
                 mock.patch.object(H, 'identify', side_effect=lambda *_a: identified.set() or True), \
                 mock.patch.object(H, 'power_handshake',
                                   side_effect=lambda *_a: handshake_done.set() or False):
                with self.assertRaises(KeyboardInterrupt):
                    H.listen_service('none', 180, 'default', True)
            self.assertEqual(reconnect_count, 1)

    def test_live_getid_change_detects_swap_without_vbus_edge(self):
        completed = threading.Event()
        reconnect_count = 0
        identify_count = 0
        mac_id = '10 0c 00 00 00 00'

        class Daemon:
            @staticmethod
            def soft_reconnect():
                nonlocal reconnect_count
                reconnect_count += 1

            @staticmethod
            def serve(_log, role_switch, ready):
                ready()
                if not completed.wait(2):
                    raise AssertionError('silent cable change was not handled')
                raise KeyboardInterrupt

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            role = base / 'role'
            role.write_text('device\n')
            runtime = base / 'runtime'
            logs = base / 'logs'
            idbus = base / 'idbus'
            runtime.mkdir()
            logs.mkdir()
            idbus.mkdir()
            (idbus / 'digital_id').write_text(mac_id + '\n')
            online = base / 'online'
            online.write_text('1\n')
            dest = logs / 'listen'
            dest.mkdir()

            def identify(*_args):
                nonlocal identify_count
                identify_count += 1
                value = mac_id if identify_count == 1 else H.CAMERA_ADAPTER_ID
                (idbus / 'digital_id').write_text(value + '\n')
                return True

            def handshake(*_args):
                completed.set()
                return True

            with mock.patch.object(H, 'ROLE', role), mock.patch.object(H, 'RUNTIME', runtime), \
                 mock.patch.object(H, 'LOGS', logs), mock.patch.object(H, 'IDBUS', idbus), \
                 mock.patch.object(H, 'POWER_ONLINE', online), \
                 mock.patch.object(H, 'LISTEN_IDENTIFY_REQUEST', runtime / 'request'), \
                 mock.patch.object(H, 'LISTEN_DIR', runtime / 'listen-dir'), \
                 mock.patch.object(H, 'ID_PROBE_INTERVAL', 0.01), \
                 mock.patch.object(H, 'preflight'), mock.patch.object(H, 'preflight_idbus'), \
                 mock.patch.object(H, 'snapshot'), \
                 mock.patch.object(H, 'new_log_dir', return_value=dest), \
                 mock.patch.object(H, 'load_iap_daemon', return_value=Daemon), \
                 mock.patch.object(H, 'identify', side_effect=identify), \
                 mock.patch.object(H, 'probe_digital_id', return_value=H.CAMERA_ADAPTER_ID) as probe, \
                 mock.patch.object(H, 'power_handshake', side_effect=handshake) as power:
                with self.assertRaises(KeyboardInterrupt):
                    H.listen_service('none', 180, 'default', True)
            self.assertTrue(probe.called)
            power.assert_called_once()
            self.assertEqual(identify_count, 2)
            self.assertEqual(reconnect_count, 2)

    def test_queued_identify_survives_listener_ready(self):
        identified = threading.Event()

        class Daemon:
            @staticmethod
            def soft_reconnect():
                pass

            @staticmethod
            def serve(_log, role_switch, ready):
                ready()
                if not identified.wait(2):
                    raise AssertionError('queued GetID was discarded during listener startup')
                raise KeyboardInterrupt

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            role = base / 'role'
            role.write_text('device\n')
            runtime = base / 'runtime'
            logs = base / 'logs'
            idbus = base / 'idbus'
            runtime.mkdir()
            logs.mkdir()
            idbus.mkdir()
            (idbus / 'digital_id').write_text(H.CAMERA_ADAPTER_ID + '\n')
            online = base / 'online'
            online.write_text('0\n')
            request = runtime / 'request'
            request.touch()
            dest = logs / 'listen'
            dest.mkdir()
            with mock.patch.object(H, 'ROLE', role), mock.patch.object(H, 'RUNTIME', runtime), \
                 mock.patch.object(H, 'LOGS', logs), mock.patch.object(H, 'IDBUS', idbus), \
                 mock.patch.object(H, 'POWER_ONLINE', online), \
                 mock.patch.object(H, 'LISTEN_IDENTIFY_REQUEST', request), \
                 mock.patch.object(H, 'LISTEN_DIR', runtime / 'listen-dir'), \
                 mock.patch.object(H, 'preflight'), mock.patch.object(H, 'preflight_idbus'), \
                 mock.patch.object(H, 'snapshot'), \
                 mock.patch.object(H, 'new_log_dir', return_value=dest), \
                 mock.patch.object(H, 'load_iap_daemon', return_value=Daemon), \
                 mock.patch.object(H, 'identify', side_effect=lambda *_a: identified.set() or True):
                with self.assertRaises(KeyboardInterrupt):
                    H.listen_service('none', 180)
            self.assertTrue(identified.is_set())
            self.assertFalse(request.exists())

    def test_listen_identify_queues_request_without_owner_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            request = Path(directory) / 'request'
            with mock.patch.object(H, 'LISTEN_IDENTIFY_REQUEST', request), \
                 mock.patch.object(H, 'preflight'), mock.patch.object(H, 'preflight_idbus'), \
                 mock.patch.object(H, 'command', return_value=(0, 'active\n')):
                H.listen_identify_main()
            self.assertTrue(request.exists())


class RecoverTests(unittest.TestCase):
    """recover STEP: one 9.6 link recovery step through debugfs, logged to NVMe."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.logs = base / 'logs'
        self.idbus = base / 'apple-tristar'
        for name in ('runtime', 'logs', 'apple-tristar'):
            (base / name).mkdir()
        for name, text in [('identify', ''), ('recover', ''),
                           ('digital_id', 'none (identify has not run)'),
                           ('digital_id_decoded', 'none'), ('idbus_frame', 'frame: none'),
                           ('reg_05', '0x00'), ('reg_0d', '0x18'), ('reg_0a', '0x00'),
                           ('recover_log', 'recover: idbus_reset\n0x0c before: 0xa3\n'
                                           '0x0c after: 0xa3')]:
            (self.idbus / name).write_text(text + '\n')
        for name, value in [('RUNTIME', base / 'runtime'), ('LOGS', self.logs),
                            ('IDBUS', self.idbus)]:
            patcher = mock.patch.object(H, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        for patcher in (mock.patch.object(H, 'preflight'), mock.patch.object(H.os, 'sync'),
                        mock.patch('sys.stdout')):
            patcher.start()
            self.addCleanup(patcher.stop)

    def run_main(self, *argv, result=(0, 'ok')):
        calls = []

        def trigger(path, text):
            calls.append((path.name, text))
            return result
        with mock.patch.object(H, 'trigger', side_effect=trigger), \
             mock.patch.object(H.sys, 'argv', ['linux-a9-usb-host', 'recover', *argv]):
            H.main()
        return calls

    def test_step_written_and_transcript_saved(self):
        for step in ('reorient', 'idbus_reset', 'fault_ack'):
            self.assertEqual(self.run_main(step), [('recover', step)])
        dest = next(p for p in self.logs.iterdir() if p.name.endswith('-recover-idbus_reset'))
        text = (dest / 'recover.txt').read_text()
        self.assertIn(f'$ echo idbus_reset > {self.idbus}/recover: ok', text)
        self.assertIn('0x0c before: 0xa3', text)
        self.assertIn('reg_0d: 0x18', text)
        self.assertTrue((self.logs / 'latest-recover.txt').is_file())

    def test_unknown_step_rejected_before_any_write(self):
        with mock.patch('sys.stderr'), self.assertRaises(SystemExit):
            self.run_main('reboot')
        self.assertEqual(list(self.logs.iterdir()), [])

    def test_missing_recover_file_names_the_entry(self):
        (self.idbus / 'recover').unlink()
        with self.assertRaisesRegex(RuntimeError, 'usb-host3'):
            self.run_main('reorient')

    def test_kernel_failure_exits_nonzero_with_transcript(self):
        with self.assertRaises(SystemExit) as raised:
            self.run_main('reorient', result=(16, 'failed: Device or resource busy'))
        self.assertEqual(raised.exception.code, 1)
        # LOGS also holds latest-recover.txt, and readdir order is not sorted.
        dest = next(p for p in self.logs.iterdir() if p.is_dir())
        self.assertIn('failed: Device or resource busy', (dest / 'recover.txt').read_text())


class ReadWriteTests(unittest.TestCase):
    def test_unique_file_readback_cleanup_preserves_existing_files(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(C.os, 'sync'):
            path = Path(directory)
            original = path / 'owner.txt'
            original.write_text('keep me')
            digest = C.check(path)
            self.assertEqual(len(digest), 64)
            self.assertEqual(original.read_text(), 'keep me')
            self.assertEqual(list(path.iterdir()), [original])

    def test_readback_failure_still_removes_test_file(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(C.os, 'sync'), mock.patch.object(Path, 'read_bytes', return_value=b'wrong'):
            with self.assertRaises(ValueError):
                C.check(Path(directory))
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_regular_file_is_never_a_block_device(self):
        with tempfile.NamedTemporaryFile() as file:
            with self.assertRaisesRegex(ValueError, 'block device'):
                C.usb_mount(file.name)

    def test_internal_device_is_never_tested(self):
        info = mock.Mock(st_mode=0o060600, st_rdev=1)
        with mock.patch.object(Path, 'resolve', return_value=Path('/sys/devices/pci/nvme/nvme0n1p2')), \
             mock.patch.object(Path, 'stat', return_value=info):
            with self.assertRaisesRegex(ValueError, 'USB ancestry'):
                C.usb_mount('/dev/nvme0n1p2')


if __name__ == '__main__':
    unittest.main(verbosity=2)
