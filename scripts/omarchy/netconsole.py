#!/usr/bin/env python3
"""Receive the iPad's kernel log (netconsole, UDP 6666) on the Mac.

  scripts/omarchy/netconsole.py [LOGFILE]   (default build/omarchy/logs/netconsole.log)

The NBD initramfs (scripts/omarchy/initramfs/init) starts the netconsole
target once the NBD server answers, so this sees every kernel message from
then on, including after the NBD root stalls. Lines are appended to LOGFILE
with the Mac's wall-clock time and echoed to stdout.
"""
import os
import socket
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/omarchy/logs/netconsole.log")
os.makedirs(os.path.dirname(path), exist_ok=True)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 6666))
print(f"netconsole: listening on UDP 6666, logging to {path}", flush=True)
with open(path, "a", buffering=1) as log:
    while True:
        data, _ = s.recvfrom(65536)
        stamp = time.strftime("%H:%M:%S")
        for line in data.decode(errors="replace").splitlines():
            out = f"{stamp} {line}"
            log.write(out + "\n")
            print(out, flush=True)
