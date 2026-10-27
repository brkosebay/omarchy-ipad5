#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Validate vendored source hashes offline; --fetch refreshes from pinned URLs."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import urllib.request


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fetch", action="store_true")
    args = ap.parse_args()
    directory = Path(__file__).with_name("upstream")
    for item in json.loads((directory / "sources.json").read_text()):
        path = directory / item["file"]
        if args.fetch:
            with urllib.request.urlopen(item["download_url"], timeout=60) as response:
                raw = response.read()
            if item.get("base64"):
                raw = base64.b64decode(raw, validate=False)
        else:
            raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != item["sha256"]:
            raise ValueError(f"{path.name}: source hash mismatch")
        if args.fetch:
            path.write_bytes(raw)
        print(f"{path.name}: {item['sha256']} PASS")


if __name__ == "__main__":
    main()
