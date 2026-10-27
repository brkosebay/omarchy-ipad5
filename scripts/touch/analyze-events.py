#!/usr/bin/env python3
"""Summarize owner's arm64 input_event hex captures without device access."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import struct


def analyze(path):
    source = path.read_bytes()
    records = []
    ignored = []
    for line in source.decode().splitlines():
        if re.fullmatch('[0-9a-fA-F]{48}', line.strip()):
            records.append(struct.unpack('<QQHHi', bytes.fromhex(line.strip())))
        elif line.strip():
            ignored.append(line.strip())
    slots = {}
    slot = 0
    starts = []
    frame_times = []
    x_values, y_values = [], []
    dropped = 0
    for sec, usec, typ, code, value in records:
        time = sec + usec / 1_000_000
        if typ == 3:
            if code == 0x2f:
                slot = value
            elif code == 0x39:
                if value >= 0:
                    contact = {'slot': slot, 'id': value, 'time': time}
                    slots[slot] = contact
                    starts.append(contact)
                else:
                    slots.pop(slot, None)
            elif code in (0x35, 0x36):
                (x_values if code == 0x35 else y_values).append(value)
                if slot in slots:
                    slots[slot]['x' if code == 0x35 else 'y'] = value
        elif typ == 0 and code == 3:
            dropped += 1
        elif typ == 0 and code == 0:
            frame_times.append(time)
            for contact in slots.values():
                if 'first_position' not in contact and 'x' in contact and 'y' in contact:
                    x, y = contact['x'], contact['y']
                    contact['first_position'] = {
                        'x': x, 'y': y, 'x_percent': round(x / 14620 * 100, 2),
                        'y_percent': round(y / 19538 * 100, 2),
                        'after_inverted_y': 19538 - y}
    gaps = [b - a for a, b in zip(frame_times, frame_times[1:])]
    return {'path': str(path), 'sha256': hashlib.sha256(source).hexdigest(),
            'events': len(records), 'frames': len(frame_times), 'syn_dropped': dropped,
            'frame_span_seconds': round(frame_times[-1] - frame_times[0], 6) if frame_times else None,
            'median_gap_ms': round(statistics.median(gaps) * 1000, 3) if gaps else None,
            'max_gap_ms': round(max(gaps) * 1000, 3) if gaps else None,
            'x_range': [min(x_values), max(x_values)] if x_values else None,
            'y_range': [min(y_values), max(y_values)] if y_values else None,
            'contact_starts': [{key: value for key, value in c.items() if key not in ('x', 'y')}
                               for c in starts],
            'non_event_lines': ignored}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', nargs='+', type=Path)
    args = parser.parse_args()
    print(json.dumps([analyze(path) for path in args.captures], indent=2))


if __name__ == '__main__':
    main()
