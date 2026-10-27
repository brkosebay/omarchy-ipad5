#!/usr/bin/env python3
"""Save focused shared-Ghidra evidence locally; never accesses the iPad."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ghidra


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('addresses', nargs='+')
    parser.add_argument('--tool', default='decompile_function_by_address')
    parser.add_argument('--length', type=int, default=256)
    parser.add_argument('--out', type=Path, default=Path('build/power/oc-r2'))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    _, sid = ghidra.post({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                         'params': {'protocolVersion': '2025-06-18', 'capabilities': {},
                                    'clientInfo': {'name': 'power-oc-re', 'version': '2'}}})
    ghidra.post({'jsonrpc': '2.0', 'method': 'notifications/initialized'}, sid)
    for index, address in enumerate(args.addresses, 2):
        params = {'address': address}
        if args.tool == 'read_memory':
            params['length'] = args.length
        result, _ = ghidra.post({'jsonrpc': '2.0', 'id': index, 'method': 'tools/call',
                                'params': {'name': args.tool, 'arguments': params}}, sid)
        if 'error' in result or result.get('result', {}).get('isError'):
            raise RuntimeError(result)
        content = '\n'.join(c.get('text', '') for c in result['result']['content'])
        target = args.out / (address + '-' + args.tool + '.json')
        target.write_text(content)
        print(target, flush=True)


if __name__ == '__main__':
    main()
