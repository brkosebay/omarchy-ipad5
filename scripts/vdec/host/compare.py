#!/usr/bin/env python3
"""Diff the driver's H.264 command stream against the reference generator.

Reads the JSON lines of vdec-h264-host (one picture each), runs
tools/ds-video/cmdstream.py --gen on every "input", and compares the
command words and the packet payloads word for word.  The generator is an
independent Python implementation of docs/periph/ds-vxd-cmdstream.md
section 16; a mismatch is a bug in one of the two, or an ambiguity in the
study.

  python3 -I scripts/vdec/host/compare.py build/vdec-r5/host/host.jsonl
"""
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
GEN = ROOT / 'tools/ds-video/cmdstream.py'


def gen(inp, text=False):
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as t:
        json.dump(inp, t)
        name = t.name
    args = [sys.executable, '-I', str(GEN), '--gen', name] + (['--text'] if text else [])
    out = subprocess.run(args, capture_output=True, text=True)
    pathlib.Path(name).unlink()
    if out.returncode:
        raise RuntimeError(out.stderr.strip() or out.stdout.strip())
    return out.stdout if text else json.loads(out.stdout)


def main():
    lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
    bad = 0
    for line in lines:
        pic = json.loads(line)
        tag = f"{pic['stream']} pic {pic['pic']} ({pic['type']}, {pic['nslices']} slice(s))"
        ref = gen(pic['input'])
        unresolved = [u['field'] for u in ref.get('unresolved', [])]
        cmd_ok = ref['cmd'] == pic['cmd']
        pk_ref = [(p['kind'], p['payload']) for p in ref['packets']]
        pk_drv = [(p['kind'], p['payload']) for p in pic['packets']]
        pk_ok = pk_ref == pk_drv
        print(f"{tag}: {len(pic['cmd'])} words, {len(pic['packets'])} packets: "
              f"cmd {'MATCH' if cmd_ok else 'DIFF'}, packets {'MATCH' if pk_ok else 'DIFF'}"
              f"{', generator complete' if ref.get('complete') else ''}"
              f"{', unresolved: ' + ', '.join(unresolved) if unresolved else ''}")
        if cmd_ok and pk_ok:
            continue
        bad += 1
        if not cmd_ok:
            text = gen(pic['input'], True).splitlines()
            a, b = pic['cmd'], ref['cmd']
            for i in range(max(len(a), len(b))):
                x = a[i] if i < len(a) else None
                y = b[i] if i < len(b) else None
                if x != y:
                    print(f"  first cmd difference at word {i} (byte {4 * i:#x}): "
                          f"driver {x if x is None else hex(x)}, generator {y if y is None else hex(y)}")
                    for t in text:
                        if t.strip().startswith(f"{4 * i:#06x}") or t.strip().startswith(f"{4 * i:06x}"):
                            print('  generator says: ' + t.strip())
                    break
            print(f"  lengths: driver {len(a)} words, generator {len(b)} words")
        if not pk_ok:
            for k, (x, y) in enumerate(zip(pk_drv, pk_ref)):
                if x != y:
                    diffs = [(4 * i, hex(u), hex(v)) for i, (u, v) in enumerate(zip(x[1], y[1])) if u != v]
                    print(f"  packet {k}: kind {x[0]} vs {y[0]}, payload diffs (offset, driver, generator): {diffs}")
            if len(pk_drv) != len(pk_ref):
                print(f"  packet count: driver {len(pk_drv)}, generator {len(pk_ref)}")
    print(f"compare: {len(lines) - bad}/{len(lines)} pictures identical -> {'PASS' if not bad and lines else 'FAIL'}")
    return 1 if bad or not lines else 0


if __name__ == '__main__':
    sys.exit(main())
