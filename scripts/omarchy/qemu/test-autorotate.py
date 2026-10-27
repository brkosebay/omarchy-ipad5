#!/usr/bin/env python3
"""Offline check of ipad-autorotate (runs on the Mac, no Hyprland, no sensor):
python3 scripts/omarchy/qemu/test-autorotate.py

Covers the classifier and its hysteresis, the IIO reader with a mount matrix,
and the daemon end to end through its mock FIFO, through a fake monitor-sensor
(the iio-sensor-proxy path) and through a fake sysfs tree, with the rotation
command replaced by a script that only writes the state file. Debounce, the
minimum interval, the lock (and unlocking), and manual rotation staying put
are checked on the way."""
import importlib.machinery
import importlib.util
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "rootfs", "files", "usr", "local", "bin")
DAEMON = os.path.join(BIN, "ipad-autorotate")
loader = importlib.machinery.SourceFileLoader("ipad_autorotate", DAEMON)
spec = importlib.util.spec_from_loader("ipad_autorotate", loader)
a = importlib.util.module_from_spec(spec)
loader.exec_module(a)

fails = 0


def check(name, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(f"{'ok ' if ok else 'FAIL'} {name}: {got}" + ("" if ok else f" (want {want})"))


# --- classifier ---------------------------------------------------------------
c = a.Classifier(20, 10)
# Expected signs are from upstream iio-sensor-proxy src/orientation.c:
# y<0 normal, y>0 bottom-up, x>0 left-up, x<0 right-up.
for name, xyz, want in [
    ("upright", (0, -9.8, 0), "normal"),
    ("upside down", (0, 9.8, 0), "bottom-up"),
    ("right-up", (-9.8, 0, 0), "right-up"),
    ("left-up", (9.8, 0, 0), "left-up"),
    ("flat, screen up: keeps", (0.2, 0.3, 9.8), "left-up"),
    ("flat, screen down: keeps", (0.2, 0.3, -9.8), "left-up"),
    ("tilted back 45 deg in a stand, upright", (0, -6.9, 6.9), "normal"),
]:
    check("classify " + name, c.feed(*xyz), want)

c = a.Classifier(20, 10)
c.feed(0, -9.8, 0)
check("normal held at 50 deg towards right", c.feed(-7.5, -6.3, 0), "normal")
check("normal -> right-up at 56 deg towards right", c.feed(-8.1, -5.4, 0), "right-up")
check("right-up held at 40 deg from it", c.feed(-6.3, -7.5, 0), "right-up")
check("right-up held at 36 deg from normal", c.feed(-5.9, -8.0, 0), "right-up")
check("right-up -> normal at 34 deg from normal", c.feed(-5.5, -8.1, 0), "normal")

# --- matrix and IIO reader ------------------------------------------------------
check("matrix: kernel format", a.parse_matrix("1, 0, 0; 0, -1, 0; 0, 0, 1"), [[1, 0, 0], [0, -1, 0], [0, 0, 1]])
check("matrix: decimals", a.parse_matrix("0.5,0,0;0,1,0;0,0,1")[0][0], 0.5)
check("matrix: wrong size", a.parse_matrix("1,0,0"), None)

tmp = tempfile.mkdtemp(prefix="autorotate-test-")
try:
    iio = os.path.join(tmp, "iio")
    dev = os.path.join(iio, "iio:device0")
    os.makedirs(dev)

    def put(name, value):
        with open(os.path.join(dev, name), "w") as f:
            f.write(str(value) + "\n")

    for axis, raw in zip("xyz", (0, 1000, 0)):
        put(f"in_accel_{axis}_raw", raw)
    put("in_accel_scale", 0.00981)
    src = a.IioSource(iio, 50, None)
    x, y, z = src.read()
    check("iio reading scaled (m/s^2)", (round(x, 2), round(y, 2), round(z, 2)), (0.0, 9.81, 0.0))
    put("in_accel_mount_matrix", "0, 1, 0; -1, 0, 0; 0, 0, 1")  # the sensor is turned a quarter
    x, y, z = src.read()
    check("kernel mount matrix applied", (round(x, 2), round(y, 2), round(z, 2)), (9.81, 0.0, 0.0))
    x, y, z = a.IioSource(iio, 50, [[1, 0, 0], [0, 1, 0], [0, 0, 1]]).read()
    check("conf matrix overrides the kernel's", (round(x, 2), round(y, 2), round(z, 2)), (0.0, 9.81, 0.0))
    os.remove(os.path.join(dev, "in_accel_mount_matrix"))

    # --- the daemon, end to end ---------------------------------------------------
    env = dict(os.environ)
    state_home = os.path.join(tmp, "state")
    conf_home = os.path.join(tmp, "config")
    run_dir = os.path.join(tmp, "run")
    for d in (os.path.join(state_home, "linux-a9"), os.path.join(conf_home, "linux-a9"), run_dir):
        os.makedirs(d)
    state = os.path.join(state_home, "linux-a9", "display")
    with open(os.path.join(conf_home, "linux-a9", "autorotate.conf"), "w") as f:
        f.write("debounce = 0.3\nmin_interval = 0.6\npoll_hz = 20\n")
    fake_rotate = os.path.join(tmp, "fake-rotate")
    calls = os.path.join(tmp, "rotate-calls")
    with open(fake_rotate, "w") as f:
        f.write(f"#!/bin/sh\necho \"$1\" >> '{calls}'\n"
                f"grep -v '^orientation=' '{state}' > '{state}.n'; echo \"orientation=$1\" >> '{state}.n'; mv '{state}.n' '{state}'\n")
    os.chmod(fake_rotate, 0o755)
    env.update(XDG_STATE_HOME=state_home, XDG_CONFIG_HOME=conf_home, XDG_RUNTIME_DIR=run_dir,
               IPAD_AUTOROTATE_ROTATE=fake_rotate)

    def set_state(key, value):
        lines = []
        if os.path.exists(state):
            lines = [ln for ln in open(state).read().splitlines() if not ln.startswith(key + "=")]
        with open(state, "w") as f:
            f.write("\n".join(lines + [f"{key}={value}"]) + "\n")

    def orientation():
        for ln in open(state).read().splitlines():
            if ln.startswith("orientation="):
                return ln.split("=", 1)[1]

    def rotations():
        return open(calls).read().split() if os.path.exists(calls) else []

    def settle(seconds=1.0):
        time.sleep(seconds)

    def start(*args, extra_env=None):
        return subprocess.Popen([sys.executable, "-B", DAEMON, *args], env={**env, **(extra_env or {})},
                                stderr=subprocess.PIPE, text=True)

    def stop(proc):
        proc.send_signal(signal.SIGTERM)
        try:
            return proc.communicate(timeout=5)[1]
        except subprocess.TimeoutExpired:
            proc.kill()
            return proc.communicate()[1]

    # (1) the mock FIFO: all four orientations, debounce, lock
    set_state("orientation", "portrait")
    fifo = os.path.join(tmp, "orient.fifo")
    d = start("--mock", fifo)
    settle(0.5)

    def feed(*lines):
        with open(fifo, "w") as f:
            f.write("\n".join(lines) + "\n")

    feed("right-up")
    settle(1.0)
    check("fifo: right-up -> landscape", orientation(), "landscape")
    feed("left-up")
    settle(1.2)
    check("fifo: left-up -> landscape-flipped", orientation(), "landscape-flipped")
    feed("bottom-up")
    settle(1.2)
    check("fifo: bottom-up -> portrait-flipped", orientation(), "portrait-flipped")
    feed("normal")
    settle(1.2)
    check("fifo: normal -> portrait", orientation(), "portrait")
    n = len(rotations())
    feed("undefined")
    settle(0.8)
    check("fifo: undefined changes nothing", len(rotations()), n)
    feed("right-up", "normal")  # flicker inside the debounce: ends where it was
    settle(1.2)
    check("fifo: flicker is debounced away", (len(rotations()), orientation()), (n, "portrait"))

    set_state("rotation_lock", "on")
    settle(0.6)
    feed("left-up")
    settle(1.2)
    check("lock: left-up is ignored", orientation(), "portrait")
    check("lock: nothing was run", len(rotations()), n)
    set_state("rotation_lock", "off")
    settle(1.2)
    check("unlock: snaps to how it is held (left-up)", orientation(), "landscape-flipped")

    set_state("orientation", "landscape")  # turned by hand
    settle(1.0)
    check("manual rotation stays until the sensor changes", orientation(), "landscape")
    set_state("orientation", "portrait")
    feed("accel -9.8 0 0")  # mounted IIO: right-up
    settle(1.2)
    check("fifo: accel line goes through the classifier", orientation(), "landscape")
    feed("accel 0 -9.8 0")
    settle(1.2)
    check("fifo: accel line, upright", orientation(), "portrait")
    log = stop(d)
    check("daemon exits on SIGTERM", d.returncode, 0)
    check("daemon logged the sensor", "sensor: right-up" in log, True)

    # (2) the iio-sensor-proxy path through a fake monitor-sensor
    set_state("orientation", "portrait")
    set_state("rotation_lock", "off")
    open(calls, "w").close()
    feed_file = os.path.join(tmp, "proxy-feed")
    open(feed_file, "w").close()
    fake_ms = os.path.join(tmp, "monitor-sensor")
    with open(fake_ms, "w") as f:
        f.write("#!/bin/sh\necho 'Waiting for iio-sensor-proxy to appear'\necho '+++ iio-sensor-proxy appeared'\n"
                "echo '=== Has accelerometer (orientation: normal, tilt: vertical)'\n"
                f"echo '=== No ambient light sensor'\nexec tail -f '{feed_file}'\n")
    os.chmod(fake_ms, 0o755)
    d = start("--source", "proxy", extra_env={"IPAD_AUTOROTATE_MONITOR_CMD": fake_ms})
    settle(1.0)
    check("proxy: initial orientation (normal) matches: no rotation", rotations(), [])
    with open(feed_file, "a") as f:
        f.write("    Accelerometer orientation changed: right-up\n")
    settle(1.3)
    check("proxy: 'Accelerometer orientation changed: right-up' -> landscape", orientation(), "landscape")
    with open(feed_file, "a") as f:
        f.write("    Accelerometer orientation changed: bottom-up\n")
    settle(1.5)
    check("proxy: bottom-up -> portrait-flipped", orientation(), "portrait-flipped")
    stop(d)

    # (3) the direct IIO path through a fake sysfs tree, with a mapping override
    with open(os.path.join(conf_home, "linux-a9", "autorotate.conf"), "a") as f:
        f.write("right-up = landscape-flipped\nleft-up = landscape\nbogus = nothing\nnormal = sideways\n")
    set_state("orientation", "portrait")
    open(calls, "w").close()
    for axis, raw in zip("xyz", (0, -1000, 0)):
        put(f"in_accel_{axis}_raw", raw)
    d = start("--mock-iio", iio)
    settle(0.8)
    check("iio: upright matches: no rotation", rotations(), [])
    for axis, raw in zip("xyz", (-1000, 0, 0)):  # right side up
        put(f"in_accel_{axis}_raw", raw)
    settle(1.3)
    check("iio: right-up, remapped by the conf -> landscape-flipped", orientation(), "landscape-flipped")
    for axis, raw in zip("xyz", (0, 0, 1000)):  # laid flat: keeps landscape-flipped
        put(f"in_accel_{axis}_raw", raw)
    settle(1.3)
    check("iio: flat changes nothing", orientation(), "landscape-flipped")
    log = stop(d)
    check("bad conf lines are reported", "sideways" in log, True)

    # (4) nothing to follow: the daemon waits instead of failing
    d = start("--source", "iio", "--mock-iio", os.path.join(tmp, "empty"))
    settle(0.8)
    check("no sensor under the iio root: still running", d.poll(), None)
    stop(d)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("FAILED" if fails else "all ok")
sys.exit(1 if fails else 0)
