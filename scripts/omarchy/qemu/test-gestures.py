#!/usr/bin/env python3
"""Offline check of ipad-gestures' recognizer and orientation mapping (runs on
the Mac: python3 scripts/omarchy/qemu/test-gestures.py)."""
import importlib.machinery
import importlib.util
import os
import sys

# No __pycache__ next to the daemon: phase linux_a9 installs everything there.
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
path = os.path.join(HERE, "..", "rootfs", "files", "usr", "local", "bin", "ipad-gestures")
loader = importlib.machinery.SourceFileLoader("ipad_gestures", path)
spec = importlib.util.spec_from_loader("ipad_gestures", loader)
g = importlib.util.module_from_spec(spec)
loader.exec_module(g)

S = g.SCREEN
S.w, S.h, S.transform = 1024, 768, 3
fails = 0


def check(name, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(f"{'ok ' if ok else 'FAIL'} {name}: {got}" + ("" if ok else f" (want {want})"))


# Orientation: transform 3 = landscape, home button on the right. The panel's
# top-left (u=0, v=0) is then the desktop's bottom-left; its bottom-left
# (home button side, u=0, v=1) is the bottom-right.
check("t3 panel top-left", S.logical(0, 0), (0, 768))
check("t3 panel bottom-left", S.logical(0, 1), (1024, 768))
check("t3 panel top-right", S.logical(1, 0), (0, 0))
S.transform = 1
check("t1 panel top-left", S.logical(0, 0), (1024, 0))
S.transform = 0
S.w, S.h = 768, 1024
check("t0 identity", S.logical(0.5, 0.25), (384, 256))
S.w, S.h, S.transform = 1024, 768, 3


def play(frames, dt=0.016):
    """frames: list of {id: (x, y)}; returns the recognised gesture."""
    r = g.Recognizer()
    t, out = 0.0, None
    for f in frames + [{}]:
        res = r.frame(t, f)
        out = res or out
        t += dt
    return out[0] if out else None


def swipe(n, dx, dy, steps=20, x0=400, y0=400):
    fr = []
    for i in range(steps + 1):
        fr.append({k: (x0 + 60 * k + dx * i / steps, y0 + dy * i / steps) for k in range(n)})
    return fr


check("3 left", play(swipe(3, -200, 0)), "swipe3-left")
check("3 right", play(swipe(3, 220, 10)), "swipe3-right")
check("3 up", play(swipe(3, 0, -150)), "swipe3-up")
check("4 up", play(swipe(4, 5, -200)), "swipe4-up")
check("4 down", play(swipe(4, 0, 200)), "swipe4-down")
check("3 tap", play(swipe(3, 0, 0, steps=8)), "tap3")
check("3 slow still: nothing", play(swipe(3, 0, 0, steps=60)), None)
# staggered landing: 1, then 2, then 3 fingers, then a swipe
stag = [{0: (400, 400)}, {0: (400, 400), 1: (460, 400)}] + swipe(3, -200, 0)
check("3 staggered left", play(stag), "swipe3-left")
pinch = [{k: (500 + (200 - 9 * i) * c, 400 + (200 - 9 * i) * s) for k, (c, s) in enumerate([(1, 0), (-0.5, 0.86), (-0.5, -0.86)])} for i in range(20)]
check("3 pinch in", play(pinch), "pinch3-in")
check("3 pinch out", play(pinch[::-1]), "pinch3-out")
check("2 fingers: apps'", play(swipe(2, -300, 0)), None)
check("edge bottom", play([{0: (500, 768 - 5 - 10 * i)} for i in range(15)]), "edge-bottom")
check("edge top", play([{0: (500, 3 + 10 * i)} for i in range(15)]), "edge-top")
check("edge left", play([{0: (5 + 10 * i, 300)} for i in range(15)]), "edge-left")
check("edge right", play([{0: (1020 - 10 * i, 300)} for i in range(15)]), "edge-right")
check("mid swipe: nothing", play([{0: (500 - 10 * i, 300)} for i in range(15)]), None)
check("long press", play([{0: (500 + i % 3, 300)} for i in range(50)]), "longpress")
check("short tap: nothing", play([{0: (500, 300)} for i in range(5)]), None)
check("long drag: nothing", play([{0: (300 + 5 * i, 300)} for i in range(50)]), None)
check("tap then another finger", play([{0: (500, 300)}] * 5 + [{1: (510, 300)}] * 50), None)
sys.exit(1 if fails else 0)
