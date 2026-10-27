#!/usr/bin/env python3
"""Test helper for ipad-gestures in the QEMU VM (or on the iPad): a uinput
MT-B touchscreen with the N1's axes (portrait 0..14620 x 0..19538) that plays
gestures given as names, laid out for the landscape desktop (transform 3).

  python3 fake-touch.py swipe3-left tap3 longpress     (FAKE_WAIT=7 to give a
                                                       running daemon time to
                                                       find the new device)
Run the daemon on it with: IPAD_GESTURES_DEVICE="Fake N1" ipad-gestures --dry-run
"""
import fcntl, os, struct, sys, time

def ioc(d, nr, size, k):
    return (d << 30) | (size << 16) | (ord(k) << 8) | nr
UI_SET_EVBIT = ioc(1, 100, 4, "U"); UI_SET_KEYBIT = ioc(1, 101, 4, "U")
UI_SET_ABSBIT = ioc(1, 103, 4, "U"); UI_SET_PROPBIT = ioc(1, 110, 4, "U")
UI_DEV_CREATE = ioc(0, 1, 0, "U")
EV = struct.Struct("llHHi")
XMAX, YMAX = 14620, 19538
fd = os.open("/dev/uinput", os.O_WRONLY)
for e in (0, 1, 3): fcntl.ioctl(fd, UI_SET_EVBIT, e)
fcntl.ioctl(fd, UI_SET_KEYBIT, 0x14A)  # BTN_TOUCH
fcntl.ioctl(fd, UI_SET_PROPBIT, 1)     # DIRECT
absmax = [0] * 64; absmin = [0] * 64
for code, mx in ((0, XMAX), (1, YMAX), (0x2F, 9), (0x35, XMAX), (0x36, YMAX), (0x39, 65535)):
    fcntl.ioctl(fd, UI_SET_ABSBIT, code); absmax[code] = mx
os.write(fd, struct.pack("80sHHHHi", b"Fake N1 Touchscreen", 3, 1, 2, 1, 0)
         + struct.pack("64i", *absmax) + struct.pack("64i", *absmin) + bytes(4 * 64 * 2))
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(float(os.environ.get("FAKE_WAIT", "1.5")))
tid = [100]

def w(t, c, v): os.write(fd, EV.pack(0, 0, t, c, v))
def syn(): w(0, 0, 0)

def play(paths, steps=20, dt=0.016, hold=0.0):
    """paths: list of ((u0,v0),(u1,v1)) panel-normalised, one per finger."""
    ids = []
    for s, _ in enumerate(paths):
        tid[0] += 1; ids.append(tid[0])
    for i in range(steps + 1):
        f = i / steps
        for s, ((u0, v0), (u1, v1)) in enumerate(paths):
            w(3, 0x2F, s)
            if i == 0: w(3, 0x39, ids[s])
            w(3, 0x35, int((u0 + (u1 - u0) * f) * XMAX)); w(3, 0x36, int((v0 + (v1 - v0) * f) * YMAX))
        if i == 0: w(1, 0x14A, 1)
        syn(); time.sleep(dt)
    if hold:
        end = time.time() + hold
        while time.time() < end:
            syn(); time.sleep(0.05)
    for s in range(len(paths)):
        w(3, 0x2F, s); w(3, 0x39, -1)
    w(1, 0x14A, 0); syn(); time.sleep(0.6)

# Landscape (transform 3): desktop x = v, desktop y = 1 - u.
def L(x, y): return (1 - y, x)   # desktop-normalised -> panel u,v
def fingers(n, x0, y0, x1, y1):
    return [(L(x0 + 0.06 * k, y0), L(x1 + 0.06 * k, y1)) for k in range(n)]

G = {
    "swipe3-left": lambda: play(fingers(3, 0.6, 0.5, 0.3, 0.5)),
    "swipe3-right": lambda: play(fingers(3, 0.3, 0.5, 0.6, 0.5)),
    "swipe4-up": lambda: play(fingers(4, 0.4, 0.7, 0.4, 0.3)),
    "tap3": lambda: play(fingers(3, 0.4, 0.5, 0.4, 0.5), steps=6),
    "edge-bottom": lambda: play([(L(0.5, 0.995), L(0.5, 0.7))]),
    "edge-left": lambda: play([(L(0.005, 0.5), L(0.3, 0.5))]),
    "longpress": lambda: play([(L(0.5, 0.5), L(0.5, 0.5))], steps=5, hold=0.8),
}
for name in sys.argv[1:]:
    print("playing", name, flush=True)
    G[name]()
time.sleep(1)
