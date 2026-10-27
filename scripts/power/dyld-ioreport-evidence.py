#!/usr/bin/env python3
"""Bounded, offline IOReport ABI/error-site disassembly; avoids huge ipsw symbol caches."""
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DSC = ROOT / 'build/dsc/com.apple.dyld/dyld_shared_cache_arm64'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


assert digest(DSC) == '276eb7cd9923e65f4ec4b60967f49ec79a847d622cbbea3323c9c0903243c79c'
print('Saved iOS 16.7 cache, libIOReport 93.100.1; virtual addresses are unslid.')
for path in (DSC, Path(str(DSC) + '.25'), Path(str(DSC) + '.42.dyldlinkedit')):
    print(f'SHA256 {digest(path)} {path.relative_to(ROOT)}')
print('IOReportCreateSubscription x4 is CFErrorRef*: failure paths store a newly created CFError through it.')
print('IOServiceOpen uses IOReportHub, type 0; then scalar selector 0, interests selector 2, telemetry buffer mapping.')
print('No entitlement-checking userspace call in this subscription routine; kernel MACF/sandbox still apply.')
for name, start, count, ranges in (
    ('IOReportCreateSubscription', 0x1d33a098c, 228,
     ((0x1d33a098c, 0x1d33a09ec), (0x1d33a0adc, 0x1d33a0b40),
      (0x1d33a0b88, 0x1d33a0ba0), (0x1d33a0bfc, 0x1d33a0d00))),
    ('IOReportCreateSamples: x2 is error output', 0x1d339f4f8, 36, None),
    ('IOReportCreateSamplesDelta: x2 captured as error output', 0x1d33a13e0, 20, None),
    ('delta callback: captured pointer passed as x3', 0x1d33a2590, 12, None),
    ('delta-channel failure: CFError stored through x19 (saved x3)', 0x1d33a20b8, 7, None),
):
    proc = subprocess.run(['ipsw', 'dyld', 'disass', str(DSC), '--quiet', '--vaddr', hex(start),
                           '--count', str(count)], check=True, capture_output=True, text=True)
    print(f'\n{name}:')
    for line in proc.stdout.splitlines():
        if not line.startswith('0x'): continue
        va = int(line.split(':', 1)[0], 16)
        if ranges is None or any(lo <= va < hi for lo, hi in ranges): print(line)
print('\nThe saved prebuilt-loader path for the registration executable is:')
print('/System/Library/PrivateFrameworks/PowerLog.framework/XPCServices/'
      'PerfPowerTelemetryClientRegistrationService.xpc/PerfPowerTelemetryClientRegistrationService')
print('String VA=0x2198f6344 in .42.dyldlinkedit, file offset=0x57f6344.')
path = Path(str(DSC) + '.42.dyldlinkedit')
with path.open('rb') as stream:
    stream.seek(0x57f6344)
    actual = stream.read(256).split(b'\0', 1)[0].decode()
assert actual == '/System/Library/PrivateFrameworks/PowerLog.framework/XPCServices/' \
                 'PerfPowerTelemetryClientRegistrationService.xpc/PerfPowerTelemetryClientRegistrationService'
