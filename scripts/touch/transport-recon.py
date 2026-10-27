#!/usr/bin/env python3
"""Save bounded offline Ghidra queries for A9 touch transport; never accesses USB."""
import argparse
import importlib.util
import json
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("name")
    ap.add_argument("tool")
    ap.add_argument("arguments", help="JSON object")
    args = ap.parse_args()
    if not args.name.replace("-", "").replace("_", "").isalnum():
        ap.error("name must be a simple file stem")
    spec = importlib.util.spec_from_file_location("ghidra_cli", root / "scripts/ghidra.py")
    client = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(client)
    client.TIMEOUT = 60
    _, sid = client.post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                     "clientInfo": {"name": "transport-recon", "version": "1"}}})
    client.post({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)
    result, _ = client.post({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                             "params": {"name": args.tool,
                                        "arguments": json.loads(args.arguments)}}, sid)
    out = root / "build/touch/transport-re" / (args.name + ".json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + "\n")
    for content in result.get("result", {}).get("content", []):
        if "text" in content:
            print(content["text"])


if __name__ == "__main__":
    main()
