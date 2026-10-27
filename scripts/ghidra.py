#!/usr/bin/env python3
"""Command-line client for the shared Ghidra MCP server (scripts/ghidra-mcp.sh).

Lets any agent or shell use Ghidra without MCP support in its own session.

  scripts/ghidra.py tools                         # list tools and their arguments
  scripts/ghidra.py get_program_info
  scripts/ghidra.py list_strings filter="META FW started" limit=5
  scripts/ghidra.py decompile_function_by_address address=fffffff006ca67e0
  scripts/ghidra.py get_xrefs_to address=0xfffffff00594350a

Arguments are key=value; values that parse as JSON (numbers, true/false)
are passed as such, everything else as a string.
"""
import json
import os
import sys
import urllib.request

URL = os.environ.get("GHIDRA_MCP_URL", "http://127.0.0.1:8765/mcp")
TIMEOUT = float(os.environ.get("GHIDRA_MCP_TIMEOUT", "900"))


def post(payload, session=None):
    headers = {"Content-Type": "application/json",
               "Accept": "application/json, text/event-stream"}
    if session:
        headers["Mcp-Session-Id"] = session
    req = urllib.request.Request(URL, json.dumps(payload).encode(), headers)
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        sid = r.headers.get("Mcp-Session-Id", session)
        body = r.read().decode()
    if not body.strip():
        return None, sid
    if body.lstrip().startswith("{"):
        return json.loads(body), sid
    msg = None  # SSE: take the last data: line carrying a JSON-RPC message
    for line in body.splitlines():
        if line.startswith("data:"):
            msg = json.loads(line[5:].strip())
    return msg, sid


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        sys.exit(__doc__)
    init, sid = post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                      "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": "ghidra.py", "version": "1"}}})
    post({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)

    if sys.argv[1] == "tools":
        res, _ = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, sid)
        for t in res["result"]["tools"]:
            args = ", ".join(t.get("inputSchema", {}).get("properties", {}))
            print(f"{t['name']}({args})")
        return

    args = {}
    for a in sys.argv[2:]:
        k, _, v = a.partition("=")
        try:
            args[k] = json.loads(v)
        except ValueError:
            args[k] = v
    res, _ = post({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                   "params": {"name": sys.argv[1], "arguments": args}}, sid)
    if res is None or "error" in res:
        sys.exit(json.dumps(res, indent=2))
    out = res["result"]
    for c in out.get("content", []):
        print(c.get("text", ""))
    if out.get("isError"):
        sys.exit(1)


if __name__ == "__main__":
    main()
