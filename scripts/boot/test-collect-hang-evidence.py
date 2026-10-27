#!/usr/bin/env python3
"""No device access: fake SSH, localhost telnet and a pseudo-terminal."""
import base64
import io
import os
from pathlib import Path
import pty
import re
import select
import socket
import subprocess
import tarfile
import tempfile
import termios
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
COLLECT = ROOT / 'scripts/boot/collect-hang-evidence.sh'


def reply(query):
    token = re.search(rb'(A9EVIDENCE_[0-9a-f]{32})_BEGIN', query)[1]
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w') as tar:
        for name, text in [('sys/fs/pstore/console-ramoops-0', b'previous console\n'),
                           ('run/linux-a9/pstore/dmesg-ramoops-0', b'PANIC fixture\n')]:
            info = tarfile.TarInfo(name)
            info.size = len(text)
            tar.addfile(info, io.BytesIO(text))
    return (b'\n' + token + b'_BEGIN\nfixture dmesg\n' + token + b'_ARCHIVE_BEGIN\n' +
            base64.encodebytes(data.getvalue()) + b'\n' + token + b'_ARCHIVE_END\n' + token + b'_END\n')


class CollectTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        bindir = self.path / 'bin'
        bindir.mkdir()
        for name in ('ioreg', 'ifconfig', 'route', 'arp'):
            f = bindir / name
            f.write_text('#!/bin/sh\necho local-fixture\n')
            f.chmod(0o755)
        self.env = dict(os.environ, PATH=str(bindir) + ':' + os.environ['PATH'], PYTHONDONTWRITEBYTECODE='1')

    def run_collector(self, *args):
        self.out = self.path / ('output-' + str(time.monotonic_ns()))
        return subprocess.Popen([str(COLLECT), '--out', str(self.out), '--seconds', '8', *args],
                                env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def assert_capture(self, process):
        stdout, stderr = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        self.assertEqual((self.out / 'records/sys/fs/pstore/console-ramoops-0').read_bytes(), b'previous console\n')
        self.assertEqual((self.out / 'records/run/linux-a9/pstore/dmesg-ramoops-0').read_bytes(), b'PANIC fixture\n')
        self.assertEqual((self.out / 'result.txt').read_text(), 'complete\n')

    def test_ssh_archive_and_read_only_query_contract(self):
        fake = self.path / 'ssh'
        fake.write_text('#!/usr/bin/env python3\nimport sys\nsys.path.insert(0,' + repr(str(Path(__file__).parent)) + ')\n'
                        'from importlib.machinery import SourceFileLoader\n'
                        'm=SourceFileLoader("fixture",' + repr(str(Path(__file__).resolve())) + ').load_module()\n'
                        'q=sys.stdin.buffer.read()\nsys.stdout.buffer.write(m.reply(q))\n')
        fake.chmod(0o755)
        self.env['OMARCHY_SSH'] = str(fake)
        self.assert_capture(self.run_collector('--transport', 'ssh'))
        queries = (self.out / 'device-queries.sh').read_text()
        self.assertNotRegex(queries, r'(^|[ ;])(?:mount|umount|rm|reboot|kexec|modprobe|dd)\s')
        self.assertNotRegex(queries.replace('>/dev/null', ''), r'(?m)[^2]>[^&]')
        self.assertNotIn('apple_ans_diag', queries)
        subprocess.run(['sh', '-n', str(self.out / 'device-queries.sh')], check=True)

    def test_telnet_split_negotiation_and_archive(self):
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen()
            port = server.getsockname()[1]
            errors = []

            def serve():
                try:
                    with server.accept()[0] as conn:
                        conn.settimeout(5)
                        conn.sendall(b'\xff')
                        time.sleep(0.05)
                        conn.sendall(b'\xfb\x01~ # ')
                        query = b''
                        while b'_END\\n' not in query:
                            query += conn.recv(65536)
                        conn.sendall(reply(query).replace(b'\n', b'\r\n'))
                except Exception as e:
                    errors.append(e)
            thread = threading.Thread(target=serve)
            thread.start()
            self.assert_capture(self.run_collector('--transport', 'telnet', '--host', '127.0.0.1', '--telnet-port', str(port)))
            thread.join(timeout=5)
            self.assertFalse(errors)

    def test_serial_multiline_queries_archive_and_terminal_restored(self):
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        original = termios.tcgetattr(slave)
        process = self.run_collector('--transport', 'serial', '--serial', os.ttyname(slave))
        query = b''
        end = time.monotonic() + 8
        while time.monotonic() < end and b'_END\\n' not in query:
            if select.select([master], [], [], 0.2)[0]:
                query += os.read(master, 65536)
        self.assertIn(b'_END\\n', query)
        self.assertLess(max(map(len, query.splitlines())), 1024)
        data = reply(query).replace(b'\n', b'\r\n')
        # Split writes exercise chunked record and marker reads.
        while data:
            sent = os.write(master, data[:137])
            data = data[sent:]
        self.assert_capture(process)
        restored = termios.tcgetattr(slave)
        # macOS may set its internal PENDIN bit when canonical input returns.
        restored[3] &= ~termios.PENDIN
        original[3] &= ~termios.PENDIN
        self.assertEqual(restored, original)

    def test_passive_serial_sends_nothing(self):
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        out = self.path / 'passive.txt'
        process = subprocess.Popen(['python3', '-B', str(ROOT / 'scripts/boot/tty-readonly.py'),
                                    os.ttyname(slave), '--passive', '--seconds', '1', '--out', str(out)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # Wait for the helper to disable echo, without sending a query.
        end = time.monotonic() + 0.7
        while termios.tcgetattr(slave)[3] & termios.ECHO and time.monotonic() < end:
            time.sleep(0.01)
        os.write(master, b'passive kernel message\n')
        stdout, stderr = process.communicate(timeout=3)
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        self.assertEqual(out.read_bytes(), b'passive kernel message\n')
        self.assertFalse(select.select([master], [], [], 0)[0], 'passive mode wrote to the device')


if __name__ == '__main__':
    unittest.main(verbosity=2)
