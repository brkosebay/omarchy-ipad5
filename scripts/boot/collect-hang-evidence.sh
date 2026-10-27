#!/usr/bin/env bash
# ORCHESTRATOR ONLY. Read-only device queries; all output is saved on the Mac.
# Python stdlib only; SSH -> initramfs telnet fallback, or explicit USB ACM.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec python3 -B - "$ROOT" "$@" <<'PY'
import argparse
import base64
import glob
import io
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import tarfile
import time
import uuid

root = Path(sys.argv[1])
p = argparse.ArgumentParser(description=__doc__ or 'Collect linux-a9 hang evidence without changing device configuration or storage.')
p.add_argument('--out', type=Path, help='new host output directory')
p.add_argument('--host', default=os.environ.get('OMARCHY_HOST', '172.16.42.1'))
p.add_argument('--transport', choices=['auto', 'ssh', 'telnet', 'serial'], default='auto')
p.add_argument('--serial', help='Mac ACM path, or auto (requires exactly one /dev/cu.usbmodem*)')
p.add_argument('--telnet-port', type=int, default=23, help='initramfs rescue port (default 23)')
p.add_argument('--passive', action='store_true', help='serial capture only: send no commands; reconnect across USB enumeration')
p.add_argument('--seconds', type=float, default=90, help='whole transport deadline / passive recording length (default 90)')
p.add_argument('--print-device-script', action='store_true', help='print read-only queries and exit without device access')
a = p.parse_args(sys.argv[2:])
if not 1 <= a.seconds <= 3600:
    p.error('--seconds must be 1..3600')
if a.passive and a.transport != 'serial':
    p.error('--passive requires --transport serial')
if a.transport == 'serial' and not a.serial:
    p.error('--transport serial requires --serial PATH|auto')

token = 'A9EVIDENCE_' + uuid.uuid4().hex
# No mount, pstore deletion, kmsg/SysRq writes, sysfs stores, block reads,
# NVMe ioctl/MMIO snapshots, retries, service changes, or reboot commands.
script = r'''
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat SYSTEMD_COLORS=0
run() {
  printf '\n--- %s\n' "$*"
  if command -v timeout >/dev/null 2>&1; then timeout 5 "$@" 2>&1
  else "$@" 2>&1; fi
  :
}
printf '\n@TOKEN@_BEGIN\n'
run date -u
run uname -a
for f in /proc/sys/kernel/random/boot_id /proc/cmdline /proc/uptime /proc/mounts \
         /proc/1/comm /proc/1/status /proc/1/wchan /proc/1/stack \
         /proc/pressure/io /proc/sys/kernel/panic /sys/kernel/kexec_loaded \
         /sys/kernel/kexec_crash_loaded /sys/kernel/kexec_crash_size \
         /sys/kernel/debug/devices_deferred /sys/kernel/debug/pm_genpd/pm_genpd_summary \
         /proc/interrupts; do
  [ ! -r "$f" ] || run cat "$f"
done
run ps -eo pid,ppid,stat,wchan:32,comm
run ip address show
run ip route show
run ip neigh show
run ss -lntp
run netstat -lnt
for f in /sys/module/apple_boot_watchdog/parameters/* \
         /sys/module/pcie_apple_h9p/parameters/ans_boot_mode \
         /sys/module/pcie_apple_h9p/parameters/probe \
         /sys/module/nvme/parameters/apple_ans_probe \
         /sys/module/nvme/parameters/write_window \
         /sys/module/nvme_core/parameters/{io_timeout,admin_timeout} \
         /sys/class/nvme/nvme*/state /sys/block/nvme*n*/stat \
         /sys/block/nvme*n*/queue/io_timeout \
         /sys/class/net/usb*/{address,operstate,carrier} \
         /sys/bus/platform/devices/610000000.pcie/ans_recover \
         /sys/bus/platform/devices/610000000.pcie/power/runtime_status \
         /sys/bus/platform/devices/601008000.iommu/power/runtime_status \
         /sys/bus/platform/devices/602008000.iommu/power/runtime_status \
         /config/usb_gadget/g1/UDC /sys/kernel/config/usb_gadget/g1/UDC; do
  [ ! -r "$f" ] || run cat "$f"
done
run ls -l /sys/bus/pci/devices /sys/class/block /sys/fs/pstore /run/linux-a9/pstore
# IRQ number is not fixed across kernels. Capture both controller queues'
# MSI identity, affinity, spurious counts and interrupt-controller state.
for f in /sys/class/nvme/nvme*/device/msi_irqs/*; do
  [ -f "$f" ] || continue
  irq=${f##*/}
  run cat "$f" "/proc/irq/$irq/spurious" "/proc/irq/$irq/smp_affinity_list"
  for state in actions chip_name hwirq type per_cpu_count; do
    [ ! -r "/sys/kernel/irq/$irq/$state" ] || run cat "/sys/kernel/irq/$irq/$state"
  done
done
for d in /proc/device-tree/reserved-memory/ramoops@*; do
  [ -d "$d" ] || continue
  for f in reg record-size console-size max-reason no-map; do
    [ ! -r "$d/$f" ] || run od -An -tx1 "$d/$f"
  done
done
run systemctl --no-pager --failed
run systemctl --no-pager list-jobs
run systemctl --no-pager status sshd.service debug-shell.service linux-a9-usb0-dhcp.service linux-a9-kexec-panic.service linux-a9-pstore.service systemd-pstore.service
run journalctl --no-pager --list-boots
run journalctl --no-pager -b -n 200 -u sshd.service -u debug-shell.service -u linux-a9-kexec-panic.service -u linux-a9-pstore.service -u systemd-pstore.service
run dmesg
run cat /HL_init.log
sleep 2
run cat /proc/interrupts
boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
paths=''
for path in sys/fs/pstore run/linux-a9/pstore var/lib/systemd/pstore "var/lib/linux-a9/pstore/$boot"; do
  [ ! -d "/$path" ] || paths="$paths $path"
done
printf '\n@TOKEN@_ARCHIVE_BEGIN\n'
if [ -n "$paths" ]; then
  # These fixed relative paths contain no whitespace. Do not read NVMe blocks.
  (cd / && if command -v timeout >/dev/null 2>&1; then timeout 15 tar -cf - $paths; else tar -cf - $paths; fi) | base64
fi
printf '\n@TOKEN@_ARCHIVE_END\n@TOKEN@_END\n'
'''.replace('@TOKEN@', token)
# BusyBox ash does not require brace expansion, and some builds lack it.
script = script.replace('/sys/module/nvme_core/parameters/{io_timeout,admin_timeout}',
                        '/sys/module/nvme_core/parameters/io_timeout /sys/module/nvme_core/parameters/admin_timeout')
script = script.replace('/sys/class/net/usb*/{address,operstate,carrier}',
                        '/sys/class/net/usb*/address /sys/class/net/usb*/operstate /sys/class/net/usb*/carrier')
if a.print_device_script:
    print(script)
    sys.exit(0)

out = a.out or root / 'build/boot/hang-evidence' / time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())
out.mkdir(parents=True, exist_ok=False)
os.chmod(out, 0o700)
(out / 'device-queries.sh').write_text(script)
print('Saving evidence to', out, flush=True)

def local(name, command):
    try:
        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
        (out / name).write_bytes(r.stdout)
    except (OSError, subprocess.TimeoutExpired) as e:
        (out / name).write_text(str(e) + '\n')

for name, command in [('host-usb.txt', ['ioreg', '-p', 'IOUSB', '-l', '-w', '0']),
                      ('host-network.txt', ['ifconfig', '-a']),
                      ('host-route.txt', ['route', '-n', 'get', a.host]),
                      ('host-arp.txt', ['arp', '-an'])]:
    local(name, command)
(out / 'host-acm.txt').write_text('\n'.join(glob.glob('/dev/cu.usbmodem*')) + '\n')

def finish(data):
    # Keep raw output even on truncation, interleaved ACM kmsg, or archive error.
    text = data.replace(b'\r', b'')
    (out / 'device.txt').write_bytes(text)
    start = (token + '_ARCHIVE_BEGIN\n').encode()
    end = ('\n' + token + '_ARCHIVE_END').encode()
    if start in text and end in text:
        encoded = text.split(start, 1)[1].split(end, 1)[0]
        if encoded.strip():
            try:
                archive = base64.b64decode(re.sub(rb'\s', b'', encoded), validate=True)
                (out / 'pstore.tar').write_bytes(archive)
                with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
                    # Do not trust archive paths or follow links supplied remotely.
                    allowed = ('sys/fs/pstore/', 'run/linux-a9/pstore/',
                               'var/lib/systemd/pstore/', 'var/lib/linux-a9/pstore/')
                    for member in tar:
                        name = member.name.removeprefix('./')
                        if not member.isfile():
                            continue
                        if '..' in Path(name).parts or not name.startswith(allowed):
                            raise ValueError('unexpected archive path: ' + name)
                        dest = out / 'records' / name
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        dest.write_bytes(tar.extractfile(member).read())
            except (ValueError, tarfile.TarError, OSError) as e:
                (out / 'archive-error.txt').write_text(str(e) + '\n')
                print('Archive incomplete; raw capture retained:', e, file=sys.stderr)
                return False
    return ('\n' + token + '_END\n').encode() in text

def ssh_capture():
    env = dict(os.environ, OMARCHY_HOST=a.host)
    command = [os.environ.get('OMARCHY_SSH', str(root / 'scripts/omarchy/ssh.sh')),
               '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
               '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2',
               'sudo -n sh -s']
    try:
        r = subprocess.run(command, input=script.encode(), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=a.seconds, env=env)
        (out / 'ssh-stderr.txt').write_bytes(r.stderr)
        (out / 'ssh-raw.txt').write_bytes(r.stdout)
        return r.returncode == 0 and finish(r.stdout)
    except subprocess.TimeoutExpired as e:
        (out / 'ssh-raw.txt').write_bytes(e.stdout or b'')
        (out / 'ssh-stderr.txt').write_bytes(e.stderr or b'')
        if e.stdout:
            finish(e.stdout)
        return False
    except OSError as e:
        (out / 'ssh-stderr.txt').write_text(str(e) + '\n')
        return False

# Keep physical input lines short: a base64-encoded script on ONE line can
# exceed the device tty's canonical input limit and silently truncate.
shell_line = ('sh -c ' + shlex.quote(script) + '\n').encode()

class TelnetFilter:
    def __init__(self):
        self.pending = b''

    def feed(self, data, sock):
        data = self.pending + data
        self.pending = b''
        clean = bytearray()
        i = 0
        while i < len(data):
            if data[i] != 255:
                clean.append(data[i]); i += 1; continue
            if i + 1 >= len(data):
                break
            cmd = data[i + 1]
            if cmd == 255:
                clean.append(255); i += 2; continue
            if cmd in (251, 252, 253, 254):
                if i + 2 >= len(data):
                    break
                if cmd in (251, 253):
                    sock.sendall(bytes([255, 254 if cmd == 251 else 252, data[i + 2]]))
                i += 3; continue
            if cmd == 250:
                j = data.find(b'\xff\xf0', i + 2)
                if j < 0:
                    break
                i = j + 2; continue
            i += 2
        self.pending = data[i:]
        return clean

def telnet_capture():
    data = bytearray()
    try:
        with socket.create_connection((a.host, a.telnet_port), timeout=5) as sock:
            sock.settimeout(1)
            parser = TelnetFilter()
            end = time.monotonic() + a.seconds
            # pmos_getty is an unauthenticated initramfs root shell, not sshd.
            # Read its prompt first so telnetd has completed the pty setup.
            try:
                data.extend(parser.feed(sock.recv(65536), sock))
            except socket.timeout:
                pass
            sock.sendall(shell_line.replace(b'\n', b'\r\n'))
            while time.monotonic() < end and len(data) < 64 * 1024 * 1024:
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                data.extend(parser.feed(chunk, sock))
                if ('\n' + token + '_END\n').encode() in data.replace(b'\r\n', b'\n'):
                    break
    except OSError as e:
        (out / 'telnet-error.txt').write_text(str(e) + '\n')
    (out / 'telnet-raw.txt').write_bytes(data)
    return finish(data) if data else False

def serial_capture():
    command = [sys.executable, '-B', str(root / 'scripts/boot/tty-readonly.py'),
               a.serial, '--seconds', str(a.seconds), '--out', str(out / 'serial-raw.txt')]
    if a.passive:
        command += ['--passive']
    else:
        command += ['--end-marker', token + '_END']
    r = subprocess.run(command, input=b'' if a.passive else shell_line, stderr=subprocess.PIPE)
    (out / 'serial-events.txt').write_bytes(r.stderr)
    data = (out / 'serial-raw.txt').read_bytes()
    return r.returncode == 0 and (a.passive or finish(data))

ok = False
if a.transport in ('auto', 'ssh'):
    ok = ssh_capture()
if not ok and a.transport in ('auto', 'telnet'):
    print('Trying initramfs telnet rescue (TCP 23).', flush=True)
    ok = telnet_capture()
if a.transport == 'serial' or (not ok and a.transport == 'auto' and a.serial):
    ok = serial_capture()
(out / 'result.txt').write_text('complete\n' if ok else 'partial or unreachable; retain raw captures\n')
print('Capture complete.' if ok else 'Partial capture; inspect raw logs and host USB/network evidence.', flush=True)
sys.exit(0 if ok else 1)
PY
