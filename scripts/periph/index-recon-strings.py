#!/usr/bin/env python3
"""Index charging/N1 diagnostic strings by kernelcache VA for Ghidra xrefs.

Reads the existing extracted sections; does not disassemble or access hardware.
"""
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/periph/round3-kexts'
TERMS = {
    'com.apple.driver.AppleD2365PMU': r'inputLimitTargetMA|chargeCurrentLimitMA|getInputCurrentSetting|setChargeCurrentLimit|usb-input-limit-calibration|attachCharger',
    'com.apple.driver.AppleARMPMU': r'PMUConfiguration|ChargerConfiguration|function-set_charger|copyAndSetAdapterDetail|setCharger|USBCurrentLimit',
    'com.apple.driver.AppleTriStar': r'CurrentLimit|charge detect|USBPower|power_supply_change',
    'com.apple.iokit.IOAccessoryManager': r'PMUConfiguration|IOAccessoryUSBCurrentLimit|IOAccessoryUSBCurrentLimitBase|setUSBCurrentLimit|USBCurrentLimit',
    'com.apple.driver.AppleMultitouchSPI': r'N1|performCalibSeq|fw-execute-addr|cal-dl-addr|prox-cal-addr|clk32-clock-enable|fll-mval',
}


def main():
    manifest = json.loads((OUT / 'manifest.json').read_text())
    matches = []
    for bundle, pattern in TERMS.items():
        for section in manifest['kexts'][bundle]['sections']:
            if section['name'] != '__cstring':
                continue
            data = (OUT / section['path']).read_bytes()
            for match in re.finditer(rb'[^\0]*\0', data):
                try:
                    text = match[0][:-1].decode('ascii')
                except UnicodeDecodeError:
                    continue
                if not re.search(pattern, text, re.I):
                    continue
                matches.append({'bundle': bundle, 'address': hex(section['va'] + match.start()),
                                'kernelcache_offset': section['kernelcache_offset'] + match.start(),
                                'text': text})
    output = ROOT / 'build/periph/round3-recon-strings.json'
    output.write_text(json.dumps({'kernelcache_sha256': manifest['kernelcache_sha256'], 'strings': matches}, indent=2) + '\n')
    for record in matches:
        print(record['address'], record['bundle'].rsplit('.', 1)[-1], record['text'].replace('\n', '\\n'))


if __name__ == '__main__':
    main()
