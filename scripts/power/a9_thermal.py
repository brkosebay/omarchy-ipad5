#!/usr/bin/env python3
"""Read-only S8003 thermal prerequisites and kernel-selected tuning profile."""
from pathlib import Path

ALL_NAMES = {f'apple-a9-{kind}{i}' for kind in ('soc', 'tcc') for i in range(3)}
# Round-3 quarantine remains in force, even after the cold-boot repair.
NAMES = {'apple-a9-soc0', 'apple-a9-soc1', 'apple-a9-tcc2'}


def read(path):
    return path.read_text().strip()


def fields(line):
    return dict(item.split('=', 1) for item in line.split() if '=' in item)


PROFILE_KEYS = ('profile', 'profile_tuning', 'passive_millic', 'hot_millic',
                'critical_millic', 'arm_millic', 'cooldown_millic')


def profile_from_raw(raw):
    header = fields(raw.splitlines()[0])
    if (header.get('format') != '3' or header.get('thermal_profile_format') != '1' or
            header.get('profile_ready') != '1' or int(header['trusted_mask'], 16) != 0x23):
        raise RuntimeError('round-5 thermal profile/mask23 interface required and ready')
    p = {key: header[key] if key == 'profile' else int(header[key]) for key in PROFILE_KEYS}
    passive = p['passive_millic']
    tuning = p['profile_tuning']
    if tuning not in (0, 1) or not 70000 <= passive <= 92000:
        raise RuntimeError('invalid kernel thermal profile bounds')
    expected = 'sustained' if passive == 90000 else 'custom'
    if not tuning:
        expected = 'stock'
    if (p['profile'] != expected or (not tuning and passive != 80000) or
            p['hot_millic'] != (95000 if tuning else 90000) or
            p['critical_millic'] != 100000 or p['arm_millic'] != passive - 5000 or
            p['cooldown_millic'] != passive - 10000):
        raise RuntimeError('inconsistent kernel thermal profile')
    return p


def _profile(root, reader):
    raw = reader(root / 'kernel/debug/a9_thermal/snapshot')
    p = profile_from_raw(raw)
    try:
        guard = fields(reader(root / 'kernel/debug/a9_tuning/status').splitlines()[0])
    except FileNotFoundError:
        if p['profile_tuning']:
            raise RuntimeError('tuning profile requires kernel guard status') from None
    else:
        if guard.get('thermal_profile_format') != '1' or any(
                guard.get(key) != str(p[key]) for key in PROFILE_KEYS):
            raise RuntimeError('kernel guard/thermal profile mismatch')
        if guard.get('enable') != str(p['profile_tuning']):
            raise RuntimeError('kernel guard/profile enable mismatch')
    return p, raw


def profile(root=Path('/sys'), *, reader=read):
    try:
        return _profile(root, reader)[0]
    except (KeyError, IndexError, ValueError) as exc:
        raise RuntimeError(f'malformed thermal profile: {exc}') from exc


def _sensors(root=Path('/sys'), *, reader=read):
    selected, raw = _profile(root, reader)
    result = {}
    for zone in sorted((root / 'class/thermal').glob('thermal_zone*')):
        name = reader(zone / 'type')
        if name not in NAMES:
            continue
        if name in result:
            raise RuntimeError(f'duplicate sensor {name}')
        try:
            temp = int(reader(zone / 'temp'))
        except OSError as exc:
            raise RuntimeError(f'{name}: required sensor unavailable: {exc}') from exc
        if not -40000 <= temp < selected['passive_millic']:
            raise RuntimeError(f'{name}: {temp} outside the trial temperature range')
        if reader(zone / 'mode') != 'enabled':
            raise RuntimeError(f'{name}: thermal zone disabled')
        trip_paths = list(zone.glob('trip_point_*_type'))
        trips = {reader(p): int(reader(p.with_name(p.name.replace('_type', '_temp'))))
                 for p in trip_paths}
        expected = {kind: selected[kind + '_millic'] for kind in ('passive', 'hot', 'critical')}
        if len(trip_paths) != 3 or trips != expected:
            raise RuntimeError(f'{name}: missing/raised trips or profile mismatch {trips}; expected {expected}')
        bound = [p for p in zone.glob('cdev*') if p.is_symlink()]
        if not any(reader(p / 'type').startswith('cpufreq-') and
                   int(reader(p / 'max_state')) > 0 for p in bound):
            raise RuntimeError(f'{name}: cpufreq cooling is not bound')
        result[name] = temp
    if set(result) != NAMES:
        raise RuntimeError(f'missing SoC zones: {sorted(NAMES - set(result))}')
    lines = raw.splitlines()
    rows = [line for line in lines[1:] if line.strip() and line.split()[0] in NAMES]
    if len(rows) != len(NAMES) or {line.split()[0] for line in rows} != NAMES:
        raise RuntimeError('incomplete thermal snapshot')
    for line in rows:
        values = fields(line)
        if (int(values['error']) or not 0 <= int(values['age_ms']) <= 500 or
                values.get('trusted') != '1' or
                not -40000 <= int(values['temp']) < selected['passive_millic'] or
                not (int(values['bk'], 16) & (1 << 31)) or
                not 0 < int(values['count'], 16) < (1 << 31)):
            raise RuntimeError(f'invalid/stale sensor: {line}')
    return result, raw


def sensors(root=Path('/sys'), *, reader=read):
    try:
        return _sensors(root, reader=reader)
    except (KeyError, IndexError, ValueError) as exc:
        raise RuntimeError(f'malformed thermal telemetry: {exc}') from exc
