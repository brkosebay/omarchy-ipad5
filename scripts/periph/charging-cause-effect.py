#!/usr/bin/env python3
"""One-boot charger/USB trace; run as root on the iPad, never from an SSH loop.

Enables DWC2 dynamic debug, follows the kernel log, and samples sysfs at 1 Hz.
No charger, Tristar, syscfg or NVRAM writes. Ctrl-C or --seconds stops capture.
The output directory must be new. Prefer /run (tmpfs) to avoid NBD log traffic.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import time

FIELDS = (
    'usb/online', 'usb/current_max', 'usb/usb_type',
    'charger/online', 'charger/input_current_limit',
    'charger/constant_charge_current', 'charger/status',
    'battery/present', 'battery/temp', 'battery/voltage_now',
    'battery/current_now',
)


def sample(sysfs):
    values = []
    for field in FIELDS:
        try:
            values.append((sysfs / field).read_text().strip())
        except OSError as error:
            values.append(f'ERR:{error.errno}')
    return values


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--seconds', type=int, default=180)
    args = ap.parse_args()
    if args.seconds <= 0:
        ap.error('--seconds must be positive')
    if os.geteuid() != 0:
        ap.error('run with sudo on the iPad')
    control = Path('/sys/kernel/debug/dynamic_debug/control')
    # Preflight the debugfs interface and dmesg before changing debug flags.
    original = control.read_text()
    subprocess.run(['dmesg', '--help'], check=True, stdout=subprocess.DEVNULL)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'dynamic-debug-before.txt').write_text(original)
    metadata = {
        'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        'kernel': os.uname().release,
        'cmdline': Path('/proc/cmdline').read_text().strip(),
        'duration_seconds': args.seconds,
        'clock': 'monotonic_ns shares the kernel printk boot-time axis',
    }
    (args.output / 'boot.json').write_text(json.dumps(metadata, indent=2) + '\n')
    # Restore only print flags that this invocation enabled, preserving other flags.
    restore = []
    for line in original.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) >= 3 and '[dwc2]' in line and 'p' not in parts[2].lstrip('='):
            restore.append(parts[0])
    stopped = False

    def stop(signum, frame):
        nonlocal stopped
        stopped = True

    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, stop)
    process = None
    with (args.output / 'kernel.log').open('w') as log:
        try:
            # --follow includes initial history, so first transitions are retained.
            process = subprocess.Popen(['dmesg', '--follow', '--time-format=raw'],
                                       stdout=log, stderr=subprocess.STDOUT)
            control.write_text('module dwc2 +p\n')
            start = time.monotonic()
            next_sample = start
            with (args.output / 'samples.tsv').open('w', buffering=1) as stream:
                writer = csv.writer(stream, delimiter='\t')
                writer.writerow(('monotonic_ns', 'realtime_ns', *FIELDS))
                while not stopped and time.monotonic() - start < args.seconds:
                    if process.poll() is not None:
                        raise RuntimeError('dmesg exited; see kernel.log')
                    writer.writerow((time.monotonic_ns(), time.time_ns(),
                                     *sample(Path('/sys/class/power_supply'))))
                    next_sample += 1
                    time.sleep(max(0, next_sample - time.monotonic()))
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            with control.open('w') as stream:
                for location in restore:
                    filename, line = location.rsplit(':', 1)
                    stream.write(f'file {filename} line {line} -p\n')
    print(args.output)


if __name__ == '__main__':
    main()
