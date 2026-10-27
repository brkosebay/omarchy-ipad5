#!/usr/bin/env python3
"""Bulk-decompile every Ghidra function in a VA range into one text file,
so a kext can be read and grepped offline (shared Ghidra MCP server).

  tools/audio/gdump.py START END OUT.c        # e.g. fffffff005e6d000 fffffff005e75f98
  tools/audio/gdump.py --calls START END OUT  # also append callee lists

Functions are found with list_functions(filter=<hex prefix>) on each 64 KiB
prefix the range touches, then filtered to [START, END).
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from ghidra import post  # noqa: E402


class G:
    def __init__(self):
        _, self.sid = post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                            "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "gdump", "version": "1"}}})
        post({"jsonrpc": "2.0", "method": "notifications/initialized"}, self.sid)
        self.n = 2

    def call(self, name, **args):
        self.n += 1
        res, _ = post({"jsonrpc": "2.0", "id": self.n, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}, self.sid)
        text = "".join(c.get("text", "") for c in res["result"].get("content", []))
        return text


def functions(g, start, end):
    found = {}
    for prefix in range(start >> 16, ((end - 1) >> 16) + 1):
        off = 0
        while True:
            j = json.loads(g.call("list_functions", filter=f"{prefix:x}", offset=off, limit=1000))
            for it in j["items"]:
                a = int(it["address"], 16)
                if start <= a < end:
                    found[a] = it["name"]
            if not j.get("has_more"):
                break
            off += 1000
    return sorted(found.items())


def main():
    argv = sys.argv[1:]
    calls = argv[0] == "--calls"
    if calls:
        argv = argv[1:]
    start, end, out = int(argv[0], 16), int(argv[1], 16), Path(argv[2])
    g = G()
    fns = functions(g, start, end)
    with out.open("w") as f:
        for a, name in fns:
            try:
                j = json.loads(g.call("decompile_function_by_address", address=f"{a:x}"))
                code = j.get("code", "")
            except Exception as e:  # keep going; note the failure
                code = f"/* decompile failed: {e} */"
            f.write(f"\n/* ==== {a:#x} {name} ==== */\n{code}\n")
            if calls:
                f.write(f"/* calls: {g.call('get_function_calls', address=f'{a:x}', direction='callees')} */\n")
    print(f"{len(fns)} functions -> {out}")


if __name__ == "__main__":
    main()
