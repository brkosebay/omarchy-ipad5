#!/usr/bin/env python3
"""Orchestrator/target only: read ten full XYZ+timestamp IIO scans.

Stop ipad-autorotate and iio-sensor-proxy first; hold the tablet still.
Restores buffer settings on success or error; does not change AOP firmware.
"""
import os
from pathlib import Path
import select
import struct
import time

devices = [p for p in Path('/sys/bus/iio/devices').glob('iio:device*')
           if (p / 'name').read_text().strip() == 'apple-a9-aop-accel']
assert len(devices) == 1
dev = devices[0]
assert (dev / 'buffer/enable').read_text().strip() == '0', 'buffer is already owned'
trigger = f'apple-a9-aop-accel-dev{dev.name.split("device")[1]}'
assert any((p / 'name').read_text().strip() == trigger
           for p in dev.parent.glob('trigger*')), 'driver trigger missing'
paths = ['trigger/current_trigger', 'buffer/length',
         *[f'scan_elements/in_accel_{a}_en' for a in 'xyz'],
         'scan_elements/in_timestamp_en']
if (dev / 'buffer/watermark').exists():
    paths.append('buffer/watermark')
saved = {p: (dev / p).read_text().strip() for p in paths}


def put(path, value):
    (dev / path).write_text(str(value) + '\n')


fd = None
try:
    put('trigger/current_trigger', trigger)
    put('buffer/length', 32)
    if 'buffer/watermark' in saved:
        put('buffer/watermark', 1)
    for axis in 'xyz':
        assert (dev / f'scan_elements/in_accel_{axis}_type').read_text().strip() == 'le:s32/32>>0'
        put(f'scan_elements/in_accel_{axis}_en', 1)
    put('scan_elements/in_timestamp_en', 1)
    fd = os.open('/dev/' + dev.name, os.O_RDONLY | os.O_NONBLOCK)
    put('buffer/enable', 1)
    data = b''
    deadline = time.monotonic() + 4
    while len(data) < 240 and time.monotonic() < deadline:
        if select.select([fd], [], [], 0.5)[0]:
            data += os.read(fd, 24 * 32)
    assert len(data) >= 240 and len(data) % 24 == 0, f'short/misaligned scans: {len(data)} bytes'
    scans = list(struct.iter_unpack('=iii4xq', data))
    assert all(a[3] < b[3] for a, b in zip(scans, scans[1:])), 'timestamps are not increasing'
    for scan in scans[:10]:
        print('x=%d y=%d z=%d timestamp_ns=%d' % scan)
    print(f'PASS {len(scans)} XYZ+timestamp scans, own trigger, 24 bytes/scan')
finally:
    put('buffer/enable', 0)
    if fd is not None:
        os.close(fd)
    for path, value in saved.items():
        put(path, value or 'NULL')
