#!/usr/bin/env python3
"""Bounded Mac ACM capture; stdin must contain only reviewed read-only queries.

Used by collect-hang-evidence.sh. --passive sends nothing and reconnects after
USB enumeration. Active mode requires an existing root shell and never resends
commands after a disconnect. No BREAK, SysRq, modem configuration or reboot.
"""
import argparse
import glob
import os
from pathlib import Path
import select
import sys
import termios
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('device', help='Mac ACM path, or auto (exactly one usbmodem)')
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--seconds', type=float, default=90)
    p.add_argument('--end-marker')
    p.add_argument('--passive', action='store_true')
    a = p.parse_args()
    if not 1 <= a.seconds <= 3600:
        p.error('--seconds must be 1..3600')
    payload = b'' if a.passive else sys.stdin.buffer.read()
    if any(len(line) > 1024 for line in payload.splitlines()):
        p.error('query input line exceeds 1024 bytes (canonical tty limit)')
    end = time.monotonic() + a.seconds
    marker = ('\n' + a.end_marker + '\n').encode() if a.end_marker else None
    tail = b''
    captured = False
    with a.out.open('wb') as log:
        while time.monotonic() < end:
            paths = sorted(glob.glob('/dev/cu.usbmodem*')) if a.device == 'auto' else [a.device]
            if len(paths) > 1:
                p.error('multiple ACM ports; specify an explicit path')
            if not paths:
                time.sleep(0.2)
                continue
            fd = None
            original = None
            try:
                fd = os.open(paths[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                original = termios.tcgetattr(fd)
                attrs = termios.tcgetattr(fd)
                # Host-side only; preserve the original settings on exit.
                attrs[0] &= ~(termios.ICRNL | termios.IXON | termios.IXOFF)
                attrs[1] &= ~termios.OPOST
                attrs[3] &= ~(termios.ECHO | termios.ICANON)
                attrs[6][termios.VMIN] = 0
                attrs[6][termios.VTIME] = 0
                termios.tcsetattr(fd, termios.TCSANOW, attrs)
                pending = b'\n' + payload if not a.passive else b''
                # Interleave sending and reading: large console output can
                # otherwise block a shell while the host is still writing.
                while time.monotonic() < end:
                    rd, wr, _ = select.select([fd], [fd] if pending else [], [], 0.2)
                    if wr:
                        # Pace short physical lines through BusyBox's shell.
                        line, sep, rest = pending.partition(b'\n')
                        chunk = line + sep
                        sent = os.write(fd, chunk)
                        pending = chunk[sent:] + rest
                    if rd:
                        chunk = os.read(fd, 65536)
                        if not chunk:
                            break
                        log.write(chunk)
                        log.flush()
                        captured = True
                        tail = (tail + chunk.replace(b'\r', b''))[-4096:]
                        if marker and marker in tail:
                            return 0
            except OSError as e:
                print(str(e), file=sys.stderr)
            finally:
                if fd is not None:
                    if original is not None:
                        try:
                            termios.tcsetattr(fd, termios.TCSANOW, original)
                        except OSError:
                            pass
                    os.close(fd)
            if not a.passive:
                break
            time.sleep(0.2)
    return 0 if a.passive and captured else 1


if __name__ == '__main__':
    sys.exit(main())
