#!/usr/bin/env python3
"""A virtual Smart Cover for the QEMU test VM (uinput), run as root in the VM:

    sudo python3 vm-switch.py FIFO [NAME]

creates an input device NAME (default "iPad Smart Cover") that reports SW_LID,
and then turns lines written to FIFO into switch events:

    closed | open           SW_LID 1 / 0
    cover2 1 | cover2 0     SW_MACHINE_COVER (reported by the device too)
    quit                    remove the device and exit

It is the stand-in for the kernel's gpio-keys-polled devices of a9/sensors, so
ipad-lid can be exercised on a real evdev node (EVIOCGSW, input_event records,
the udev uaccess rule) and a real Hyprland.
"""
import fcntl
import os
import struct
import sys
import time

EV_SYN, EV_SW = 0, 5
SW_LID, SW_MACHINE_COVER = 0, 0x10


def _io(nr):
    return (ord("U") << 8) | nr


def _iow(nr, size):
    return (1 << 30) | (size << 16) | (ord("U") << 8) | nr


UI_DEV_CREATE = _io(1)
UI_DEV_DESTROY = _io(2)
UI_DEV_SETUP = _iow(3, 92)  # struct uinput_setup: input_id (8) + name[80] + ff_effects_max (4)
UI_SET_EVBIT = _iow(100, 4)
UI_SET_SWBIT = _iow(109, 4)
EVENT = struct.Struct("llHHi")


def emit(fd, code, value):
    now = time.time()
    sec, usec = int(now), int((now % 1) * 1e6)
    os.write(fd, EVENT.pack(sec, usec, EV_SW, code, value) + EVENT.pack(sec, usec, EV_SYN, 0, 0))


def main():
    fifo = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else "iPad Smart Cover"
    if not os.path.exists(fifo):
        os.mkfifo(fifo, 0o666)
    os.chmod(fifo, 0o666)
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SW)
    fcntl.ioctl(fd, UI_SET_SWBIT, SW_LID)
    fcntl.ioctl(fd, UI_SET_SWBIT, SW_MACHINE_COVER)
    setup = struct.pack("HHHH80sI", 0x03, 0x1209, 0xA9A9, 1, name.encode(), 0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    print(f"vm-switch: created '{name}', commands on {fifo}", flush=True)
    try:
        # Read-write, so the FIFO never reports EOF when a writer closes.
        with os.fdopen(os.open(fifo, os.O_RDWR), "r") as f:
            for line in f:
                cmd = line.strip().lower()
                if cmd == "quit":
                    break
                if cmd == "closed":
                    emit(fd, SW_LID, 1)
                elif cmd == "open":
                    emit(fd, SW_LID, 0)
                elif cmd in ("cover2 1", "cover2 0"):
                    emit(fd, SW_MACHINE_COVER, int(cmd[-1]))
                print("vm-switch:", cmd, flush=True)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)


if __name__ == "__main__":
    main()
