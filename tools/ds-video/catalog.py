#!/usr/bin/env python3
"""Build a method/string catalogue for one iOS kext out of the shared Ghidra
kernelcache analysis.

Every kext compiled by Apple carries its `__PRETTY_FUNCTION__`-style signature
strings for logging and assertions, and each one is referenced from exactly
the method it belongs to. Walking string -> xref therefore recovers a method
map (name + linked address) for a kext with no symbol table, which is what
lets this repo quote kernelcache addresses as evidence.

  tools/ds-video/catalog.py scaler
  tools/ds-video/catalog.py jpeg --mode sig
  tools/ds-video/catalog.py jpeg --mode msg --limit 40
  tools/ds-video/catalog.py --filter M2ScalerCSCRequest --table

Talks to the same server as scripts/ghidra.py (GHIDRA_MCP_URL). Output is
markdown so it can be pasted straight into docs/periph/.
"""
import argparse
import json
import os
import sys
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts"))
URL = os.environ.get("GHIDRA_MCP_URL", "http://127.0.0.1:8765/mcp")

PROFILES = {
    "scaler": ["AppleM2ScalerCSC", "M2ScalerCSC", "IOSurfaceAccelerator", "M2Scaler"],
    "jpeg": ["AppleJPEG"],
}


class MCP:
    def __init__(self, url=URL):
        self.url = url
        self.sid = None
        self.n = 0
        self._post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                               "clientInfo": {"name": "ds-video-catalog", "version": "1"}}})
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _post(self, payload):
        headers = {"Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream"}
        if self.sid:
            headers["Mcp-Session-Id"] = self.sid
        req = urllib.request.Request(self.url, json.dumps(payload).encode(), headers)
        with urllib.request.urlopen(req, timeout=900) as r:
            sid = r.headers.get("Mcp-Session-Id")
            if sid:
                self.sid = sid
            body = r.read().decode()
        if not body.strip():
            return None
        if body.lstrip().startswith("{"):
            return json.loads(body)
        msg = None
        for line in body.splitlines():
            if line.startswith("data:"):
                msg = json.loads(line[5:].strip())
        return msg

    def call(self, name, **args):
        self.n += 1
        res = self._post({"jsonrpc": "2.0", "id": self.n + 10, "method": "tools/call",
                          "params": {"name": name, "arguments": args}})
        if res is None:
            return None
        if "error" in res:
            raise RuntimeError(f"{name}: {res['error']}")
        out = res["result"]
        texts = [c.get("text", "") for c in out.get("content", [])]
        if out.get("isError"):
            raise RuntimeError(f"{name}: {texts}")
        txt = texts[0] if texts else ""
        try:
            return json.loads(txt)
        except ValueError:
            return txt


def strings_for(mcp, prefixes):
    seen, out = set(), []
    for p in prefixes:
        d = mcp.call("list_strings", filter=p, limit=2000)
        for it in (d or {}).get("items", []):
            if it["address"] in seen:
                continue
            seen.add(it["address"])
            out.append(it)
    out.sort(key=lambda i: int(i["address"], 16))
    return out


def classify(value):
    if "::" in value and value.rstrip().endswith(")"):
        return "sig"
    return "msg"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", nargs="?", default="scaler", choices=sorted(PROFILES))
    ap.add_argument("--filter", default="", help="extra substring filter on the string value")
    ap.add_argument("--mode", choices=["all", "sig", "msg"], default="all")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--table", action="store_true", help="markdown table instead of list")
    ap.add_argument("--json", default="", help="also write the raw rows here")
    args = ap.parse_args()

    mcp = MCP()
    rows = []
    for it in strings_for(mcp, PROFILES[args.profile]):
        value = it["value"].replace("\n", "\\n")
        if args.filter and args.filter not in value:
            continue
        kind = classify(value)
        if args.mode != "all" and kind != args.mode:
            continue
        xr = mcp.call("get_xrefs_to", address=it["address"], limit=8) or {}
        fns, addrs = [], []
        for x in xr.get("items", []):
            f = x.get("from_function") or {}
            n = f.get("name") or "-"
            if n not in fns:
                fns.append(n)
            if f.get("address"):
                addrs.append(f["address"])
        rows.append({"addr": it["address"], "kind": kind, "value": value,
                     "from": xr.get("items", [])[0]["from"] if xr.get("items") else "",
                     "functions": fns})
        if args.limit and len(rows) >= args.limit:
            break

    if args.json:
        with open(args.json, "w") as f:
            json.dump(rows, f, indent=1)
        print(f"# wrote {len(rows)} rows to {args.json}", file=sys.stderr)

    if args.table:
        print("| string | kind | linked address | referenced from |")
        print("| --- | --- | --- | --- |")
        for r in rows:
            fn = r["functions"][0] if r["functions"] else "-"
            val = r["value"].replace("|", "\\|")
            print(f"| `{val}` | {r['kind']} | `{r['addr']}` | `{fn}` ({r['from']}) |")
    else:
        for r in rows:
            fn = r["functions"][0] if r["functions"] else "-"
            print(f"{r['addr']}  [{r['kind']}]  {r['value']}")
            print(f"    <- {fn} @ {r['from']}")


if __name__ == "__main__":
    main()
