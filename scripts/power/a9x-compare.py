#!/usr/bin/env python3
"""Summarize saved public A9X ADTs, without device or network access.

Prepare using scripts/get-adt.sh iPad6,3 iPad6,7. Output includes only the
requested placeholder-table fields and file hashes, never calibration.
"""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ('iPad6,3', ['j127', 'j128'],
     'https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-76793/'
     '64DF3BE2-15B0-4C3D-9E4F-9E901916C71D/iPadPro_9.7_16.7.16_20H392_Restore.ipsw'),
    ('iPad6,7', ['j98a', 'j99a'],
     'https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77150/'
     '627F6C0C-E3B9-4601-A438-E7A3C2401848/iPadPro_12.9_16.7.16_20H392_Restore.ipsw'),
]
EXPECTED = {
    'j127': 'dc7193ee0665d396cdc0e08e722395f8c63c84c66391f71ae8e28992258778be',
    'j128': 'a6e38e5c65eed5ee7e2bdd52b6304e51aae4eeeb9d040e7d928e9d84c75b747c',
    'j98a': '4311bbb1098c0d1f8671b400764515fb387d0d1f7be90f103444189bcb47fa1b',
    'j99a': '588a2be5c2dcf8c583b8f1ff858dada4d0d6626bd0934aa7119c50fb8f42d0f4',
}


def find(node, key):
    if isinstance(node, dict):
        if key in node:
            return node[key]
        for value in node.values():
            result = find(value, key)
            if result is not None:
                return result
    elif isinstance(node, list):
        for value in node:
            result = find(value, key)
            if result is not None:
                return result
    return None


def main():
    output = {'note': 'Unbooted public ADTs; zeros cannot estimate live per-die voltages.',
              'sources': []}
    for device, boards, url in SOURCES:
        for board in boards:
            path = ROOT / 'docs/adt' / (board + '.json')
            raw = path.read_bytes()
            tree = json.loads(raw)
            pmgr = find(tree, 'pmgr')
            # Do not silently label a different build with the recorded CDN URL.
            digest = hashlib.sha256(raw).hexdigest()
            if digest != EXPECTED[board]:
                raise ValueError(f'{board}: ADT differs from recorded 20H392 input; reverify provenance')
            output['sources'].append({
                'product_type': device, 'board': board, 'version': '16.7.16',
                'build': '20H392', 'ipsw_url': url,
                'adt_json_sha256': digest,
                'compatible': pmgr['compatible'],
                'voltage_states0': pmgr.get('voltage-states0'),
                'voltage_states1': pmgr.get('voltage-states1')})
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
