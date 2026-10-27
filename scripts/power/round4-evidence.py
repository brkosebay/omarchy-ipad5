#!/usr/bin/env python3
"""Summarize existing cold-boot logs and hash-pinned static evidence; no iPad IO."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/power/oc-r4'
PIN = 'd5a10ac52a6468484854419a6c5130f1d62073eb'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disassemble', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    from a9_thermal import fields, NAMES
    log = ROOT / 'docs/power/logs/2026-10-23-r3-tri-boot-thermal-check.jsonl'
    events = [json.loads(line) for line in log.read_text().splitlines()]
    samples = [row for row in events if row['event'] == 'thermal-sample']
    assert samples and events[-1]['event'] == 'thermal-check-pass'
    pairs = sorted({(r['guard']['voltage_ctl'], r['guard']['voltage_routes']) for r in samples})
    assert pairs == [('00000000', '00001782')]
    for sample in samples:
        assert set(sample['temperatures_millic']) == NAMES
        assert fields(sample['thermal_raw'].splitlines()[0])['trusted_mask'] == '23'
    failure = ROOT / 'docs/power/logs/2026-10-23-r3-guard-check-eopnotsupp.txt'
    text = failure.read_text()
    assert '[Errno 95]' in text
    state = fields(text.splitlines()[1])
    regs = fields(text.splitlines()[2])
    assert state['enable'] == '1' and state['armed'] == '0' and state['dirty'] == '0'
    assert regs['voltage_ctl'] == '00000000' and regs['voltage_routes'] == '00001782'
    sources = {}
    for name in ('cpufreq.c', 'payload.c'):
        url = f'https://raw.githubusercontent.com/HoolockLinux/m1n1/{PIN}/src/{name}'
        path = OUT / ('m1n1-' + name)
        if not path.exists():
            path.write_bytes(urllib.request.urlopen(url).read())
        sources[name] = {'url': url, 'sha256': sha(path)}
    assert 'write32(pmgr_reg + PMGR_VOLTAGE_CTL_OFF_S8000, 1);' in (OUT / 'm1n1-cpufreq.c').read_text()
    assert 'if (kernel && fdt) {\n        cpufreq_init();' in (OUT / 'm1n1-payload.c').read_text()
    previous = json.loads((ROOT / 'docs/power/evidence/overclock-round3.json').read_text())
    ios = json.loads((OUT / 'fffffff005e91b0c-disassemble_function.json').read_text())
    ios_excerpt = [row for row in ios['items'] if row['address'] in (
        'fffffff005e91b24', 'fffffff005e91b34', 'fffffff005e91b3c',
        'fffffff005e91b80', 'fffffff005e91b8c', 'fffffff005e91bac', 'fffffff005e91c04')]
    assert len(ios_excerpt) == 7
    excerpts = {}
    ranges = {'iboot': [('0x87001b7b4', 16), ('0x87001cebc', 12), ('0x87001c968', 36)],
              'llb': [('0x18039655c', 20), ('0x180396978', 16), ('0x18039750c', 12)]}
    for key in ('iboot', 'llb'):
        record = previous[key]
        path = ROOT / record['file']
        assert sha(path) == record['sha256']
        data, base = path.read_bytes(), int(record['load_base'], 16)
        excerpts[key] = {'sha256': sha(path), 'ranges': []}
        for address, length in ranges[key]:
            start = int(address, 16) - base
            raw = data[start:start + length]
            entry = {'address': address, 'bytes': raw.hex()}
            if args.disassemble:
                from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
                cs = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
                entry['instructions'] = [f'{i.address:x}: {i.mnemonic} {i.op_str}'
                                         for i in cs.disasm(raw, int(address, 16))]
            excerpts[key]['ranges'].append(entry)
    # A static write-of-1 is not a post-write readback assertion. Apple code
    # separately uses a five-bit mask; this round does not claim to decode bit0.
    result = {'device_access': 'none; supplied logs only', 'm1n1': sources,
        'bootloader_excerpts': excerpts,
        'ios_mode_helper': {'address': '0xfffffff005e91b0c', 'instructions': ios_excerpt},
        'sample_count': len(samples), 'observed_voltage_ctl_routes': pairs,
        'actual_khz_observed': sorted({r['actual_khz'] for r in samples}),
        'required_sensors': sorted(NAMES), 'thermal_pass': events[-1],
        'arm_refusal': {'kernel_ref': 'b553f1ebaf', 'function': 'a9_arm',
                       'check': 'voltage_ctl != 1 || voltage_routes != 0x1782',
                       'errno': 95, 'before_sensor_check_or_table_store': True},
        'interpretation': '0/1782 is the observed Linux stock handoff, not a status formatting error; exact-1 precondition was unsupported',
        'unknowns': ['literal pre-m1n1 iBoot control value is not in these logs',
                     'why the m1n1 write of 1 is later read as 0 (self-clearing or intervening change not proved)',
                     'analog rail response/settling is not established by PSINFO readback'],
        'inputs': {str(p.relative_to(ROOT)): sha(p) for p in (log, failure,
            OUT / 'fffffff005e91b0c-disassemble_function.json')}}
    target = ROOT / 'docs/power/evidence/overclock-round4.json'
    if target.exists() and not args.disassemble:
        old = json.loads(target.read_text())
        for key in excerpts:
            for entry, prior in zip(excerpts[key]['ranges'], old['bootloader_excerpts'][key]['ranges']):
                if entry['bytes'] == prior['bytes'] and 'instructions' in prior:
                    entry['instructions'] = prior['instructions']
    target.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{target}: {len(samples)} cold-boot samples; handoff 0/1782; errno 95 traced')


if __name__ == '__main__':
    main()
