#!/usr/bin/env python3
"""Offline Ghidra analysis of an extracted AppleD5500 (or sibling) Mach-O.

The shared Ghidra MCP project is the whole kernelcache: its __text only covers
the main kernel, so kext bodies come back FUNCTION_NOT_FOUND there.  This
builds a *separate*, local project holding just the extracted kext, lets Ghidra
analyse it, and dumps decompilation so the protocol work can grep it.

Run with the ghidra-mcp virtualenv's python (PyGhidra inside):

  ~/.local/share/uv/tools/ghidra-mcp/bin/python -I tools/ds-video/ghidra-kext.py \
      --macho build/video/kexts/AppleD5500.macho \
      --project build/video/ghidra-d5500 --dump build/video/d5500-decomp.txt

Options:
  --list            print function inventory only (address, size, name)
  --func ADDR       decompile one function (repeatable)
  --analyze-only    build/analyse the project, dump nothing
Everything lands under build/ (gitignored); the kext's linked VAs are kept, so
addresses here match docs/periph/ds-video-decoder.md.
"""
import argparse
import pathlib
import sys

import pyghidra

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--macho', default='build/video/kexts/AppleD5500.macho')
    ap.add_argument('--project', default='build/video/ghidra-d5500')
    ap.add_argument('--dump', default='')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--func', action='append', default=[])
    ap.add_argument('--analyze', action='store_true',
                    help='run Ghidra auto-analysis (slow, first time)')
    args = ap.parse_args()

    macho = (ROOT / args.macho).resolve()
    loc = (ROOT / args.project).resolve()
    loc.mkdir(parents=True, exist_ok=True)

    with pyghidra.open_program(macho, project_location=loc,
                               project_name=loc.name,
                               analyze=args.analyze,
                               program_name=macho.stem) as flat:
        program = flat.getCurrentProgram()

        if not args.analyze:
            from ghidra.app.plugin.core.analysis import AutoAnalysisManager
            from ghidra.util.task import TaskMonitor
            mgr = AutoAnalysisManager.getAnalysisManager(program)
            mgr.startAnalysis(TaskMonitor.DUMMY)
            print('analysis complete', file=sys.stderr)

        fm = program.getFunctionManager()
        funcs = sorted(fm.getFunctions(True), key=lambda f: f.getEntryPoint().getOffset())
        print(f'{len(funcs)} functions', file=sys.stderr)

        if args.list:
            for f in funcs:
                body = f.getBody()
                print(f'{f.getEntryPoint()} {body.getNumAddresses():6d} {f.getName()}')
            return

        if args.func:
            from ghidra.app.decompiler import DecompInterface
            dec = DecompInterface()
            dec.openProgram(program)
            for a in args.func:
                addr = flat.toAddr(a)
                f = fm.getFunctionAt(addr) or fm.getFunctionContaining(addr)
                if f is None:
                    print(f'== {a}: no function ==')
                    continue
                res = dec.decompileFunction(f, 60, None)
                print(f'== {f.getEntryPoint()} {f.getName()} ==')
                print(res.getDecompiledFunction().getC() if res.decompileCompleted()
                      else 'DECOMPILE FAILED')
            return

        if not args.dump:
            return

        from ghidra.app.decompiler import DecompInterface
        dec = DecompInterface()
        dec.openProgram(program)
        out = (ROOT / args.dump).resolve()
        n = 0
        with out.open('w') as fh:
            for f in funcs:
                try:
                    res = dec.decompileFunction(f, 90, None)
                except Exception:
                    continue
                if not res.decompileCompleted():
                    continue
                body = res.getDecompiledFunction().getC()
                fh.write(f'\n/* ===== {f.getEntryPoint()} {f.getName()} '
                         f'({f.getBody().getNumAddresses()} bytes) ===== */\n')
                fh.write(body)
                n += 1
        print(f'wrote {n} functions to {out}', file=sys.stderr)


if __name__ == '__main__':
    main()
