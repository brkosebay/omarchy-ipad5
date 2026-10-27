#!/usr/bin/env python3
"""Run the unmodified Image through setup_arch to console_init in QEMU TCG.

This is a boot-phase test, not Apple hardware emulation. QEMU virt supplies
GIC/timer; the FDT exposes j71t's high RAM range and no-map ANS HMB. A tiny
loader sets x0 to that FDT and enters the Image at 0x800200000 in EL1. Only
the exposed ~2 GiB is touched; QEMU reserves 33 GiB of sparse host VA so this
physical range exists. No disks, network, USB or device connections are used.

The production config has no PL011 console. GDB breakpoints therefore prove
arrival at console_init without adding any config or Image instrumentation.
Export symbols with: llvm-nm -n /src/wt/TREE/vmlinux > .../symbols.txt
"""
import argparse
import gzip
import importlib.machinery
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time

FDT = importlib.machinery.SourceFileLoader(
    'qemu_fdt', str(Path(__file__).parent/'device/linux-a9-fdt-merge')).load_module()
BASE, ENTRY, DTB = 0x800000000, 0x800200000, 0x87b000000


class Remote:
    def __init__(self, path, process):
        self.sock = socket.socket(socket.AF_UNIX)
        deadline = time.monotonic()+10
        while True:
            try:
                self.sock.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                assert process.poll() is None, 'QEMU exited before GDB connected'
                if time.monotonic() >= deadline:
                    raise TimeoutError('QEMU GDB socket did not open')
                time.sleep(0.05)
        self.sock.settimeout(30)

    def packet(self, command):
        payload = command.encode()
        self.sock.sendall(b'$'+payload+b'#'+f'{sum(payload)%256:02x}'.encode())
        while True:
            ch = self.sock.recv(1)
            if not ch:
                raise RuntimeError('QEMU closed GDB socket')
            if ch != b'$':
                continue
            data = bytearray()
            while (ch := self.sock.recv(1)) != b'#':
                if not ch:
                    raise RuntimeError('truncated GDB packet')
                data.extend(ch)
            checksum = self.sock.recv(2)
            assert int(checksum, 16) == sum(data)%256, 'GDB checksum mismatch'
            self.sock.sendall(b'+')
            # Ignore remote console output packets, if any.
            if data.startswith(b'O') and data != b'OK':
                continue
            return data.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--symbols', type=Path, required=True)
    parser.add_argument('--expect', choices=['console', 'fault'], default='console')
    parser.add_argument('--control', action='store_true', help='omit j71t compatible to bypass its early reservation')
    parser.add_argument('--log', type=Path, required=True)
    args = parser.parse_args()
    assert not args.control or args.expect == 'console', 'the control must reach console_init'
    qemu = shutil.which('qemu-system-aarch64')
    assert qemu, 'qemu-system-aarch64 is required'
    symbols = {name: int(addr, 16) for line in args.symbols.read_text().splitlines()
               if len(parts := line.split()) == 3 for addr, _, name in [parts]}
    stops = {symbols[n]: n for n in ['console_init', 'el1h_64_sync']}
    image = args.image.read_bytes()
    if image.startswith(b'\x1f\x8b'):
        image = gzip.decompress(image)
    assert image[56:60] == b'ARM\x64', 'not an arm64 Image'
    assert (ENTRY-struct.unpack_from('<Q', image, 8)[0]) % 0x200000 == 0, 'Image load alignment disagrees with text offset'
    with tempfile.TemporaryDirectory(prefix='a9-early-') as d:
        tmp = Path(d)
        common = [qemu, '-machine', 'virt,gic-version=2,virtualization=off',
                  '-cpu', 'max', '-m', '33G', '-smp', '1', '-display', 'none',
                  '-serial', 'none', '-monitor', 'none', '-nic', 'none']
        subprocess.run([*common, '-machine', f'dumpdtb={tmp}/virt.dtb'], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=15)
        tree, rsv, cpu = FDT.parse((tmp/'virt.dtb').read_bytes())
        tree.props['compatible'] = b'linux-a9,qemu-control\0' if args.control else b'apple,j71t\0'
        tree.props['model'] = b'linux-a9 offline high-RAM boot-phase fixture\0'
        memory = tree.children.pop('memory@40000000')
        memory.name = 'memory@800000000'
        memory.props['reg'] = struct.pack('>QQ', BASE, 0x7bbe4000)
        tree.children[memory.name] = memory
        reserved = FDT.Node('reserved-memory')
        reserved.props = {'#address-cells': struct.pack('>I', 2),
                          '#size-cells': struct.pack('>I', 2), 'ranges': b''}
        hmb = FDT.Node('nvme-hmb@87ee00000')
        hmb.props = {'reg': struct.pack('>QQ', 0x87ee00000, 0x1200000), 'no-map': b''}
        reserved.children[hmb.name] = hmb
        tree.children[reserved.name] = reserved
        tree.children['chosen'].props['bootargs'] = b'nokaslr console=tty0 panic=0 crashkernel=256M crash_kexec_post_notifiers\0'
        (tmp/'test.dtb').write_bytes(FDT.serialize(tree, rsv, cpu))
        (tmp/'Image').write_bytes(image)
        # ldr x0, literal; mov x1/x2/x3, xzr; ldr x4, literal; br x4;
        # two 64-bit literals follow at offsets 24 and 32.
        (tmp/'entry.bin').write_bytes(struct.pack('<6I2Q', 0x580000c0, 0xaa1f03e1,
            0xaa1f03e2, 0xaa1f03e3, 0x58000084, 0xd61f0080, DTB, ENTRY))
        command = [*common, '-S', '-gdb', f'unix:{tmp}/gdb,server=on,wait=off',
                   '-d', 'int', '-D', str(tmp/'exceptions.log'),
                   '-device', f'loader,file={tmp}/Image,addr={ENTRY:#x},force-raw=on',
                   '-device', f'loader,file={tmp}/test.dtb,addr={DTB:#x},force-raw=on',
                   '-device', f'loader,file={tmp}/entry.bin,addr={BASE:#x},cpu-num=0,force-raw=on']
        with (tmp/'stderr').open('w') as err:
            proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=err)
            remote = None
            try:
                remote = Remote(tmp/'gdb', proc)
                remote.packet('qSupported')
                for addr in stops:
                    assert remote.packet(f'Z1,{addr:x},4') == 'OK'
                reply = remote.packet('c')
                assert reply.startswith('T05') or reply == 'S05', f'unexpected stop {reply}'
                regs = bytes.fromhex(remote.packet('g'))
                pc = struct.unpack_from('<Q', regs, 32*8)[0]
                x0 = struct.unpack_from('<Q', regs)[0]
                lr = struct.unpack_from('<Q', regs, 30*8)[0]
                reached = stops.get(pc, f'unknown PC {pc:#x}')
                if reached == 'console_init' and not args.control:
                    def read(address, size):
                        data = bytearray()
                        while len(data) < size:
                            count = min(1024, size-len(data))
                            reply = remote.packet(f'm{address+len(data):x},{count:x}')
                            chunk = bytes.fromhex(reply)
                            assert len(chunk) == count, 'short GDB memory read'
                            data.extend(chunk)
                        return bytes(data)
                    pointer = struct.unpack('<Q', read(symbols['initial_boot_params'], 8))[0]
                    physical = struct.unpack('<Q', read(symbols['initial_boot_params_pa'], 8))[0]
                    size = struct.unpack_from('>I', read(pointer, 40), 4)[0]
                    assert 40 <= size <= 0x200000, 'bad published FDT size'
                    live, reservations, _ = FDT.parse(read(pointer, size))
                    nodes = [n for n in live.children['reserved-memory'].children.values()
                             if n.props.get('compatible') == b'ramoops\0']
                    assert len(nodes) == 1, 'ramoops missing or duplicated at console_init'
                    base, length = struct.unpack('>QQ', nodes[0].props['reg'])
                    assert length == 0x200000 and base % length == 0
                    assert BASE <= base < base+length <= BASE+0x7bbe4000
                    assert 'no-map' in nodes[0].props and (base, length) in reservations
                    assert physical != DTB and pointer != physical, 'replacement FDT was not published'
                    print(f'published FDT pa={physical:#x}; retained ramoops={base:#x}+{length:#x}, no-map and memreserve present')
            finally:
                if remote:
                    remote.sock.close()
                proc.terminate()  # Only the QEMU PID this test started.
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait(timeout=5)
                diagnostics = (tmp/'exceptions.log').read_text()+(tmp/'stderr').read_text()
                args.log.write_text(diagnostics)
        print(f'stop={reached} pc={pc:#x} x0={x0:#x} lr={lr:#x} control={args.control}')
        if args.expect == 'console':
            assert reached == 'console_init', f'pre-console fault; inspect {args.log}'
        else:
            assert reached == 'el1h_64_sync' and 'Data Abort' in diagnostics, f'expected old pre-paging data abort; inspect {args.log}'
            fault_pc = int(re.search(r'with ELR (0x[0-9a-f]+)', diagnostics)[1], 16)
            memset_start = symbols['__pi_memset_generic']
            allocator = symbols['memblock_alloc_try_nid']
            assert memset_start <= fault_pc < symbols['memset'], 'fault is outside the generic memset implementation'
            assert allocator < lr < allocator+0x200, 'fault did not come from the memblock virtual allocator'
            print(f'fault=__pi_memset_generic+{fault_pc-memset_start:#x}; caller=memblock_alloc_try_nid+{lr-allocator:#x}')
        print(f'PASS expected {args.expect}; Image unchanged, high RAM 0x800000000..0x87bbe4000; exception log {args.log}')


if __name__ == '__main__':
    main()
