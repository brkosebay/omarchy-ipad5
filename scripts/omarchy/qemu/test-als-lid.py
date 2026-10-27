#!/usr/bin/env python3
"""Offline check of ipad-als-brightness and ipad-lid (runs on the Mac, no
Hyprland, no sensor, no cover):  python3 scripts/omarchy/qemu/test-als-lid.py

ipad-als-brightness: the curve, the smoothing and the hysteresis; then the
daemon end to end through its mock FIFO, a fake monitor-sensor (the
iio-sensor-proxy path) and a fake sysfs tree, with brightnessctl replaced by a
script that writes a fake backlight. Checked on the way: off until switched on,
small changes ignored, spikes rejected, a manual change pausing it and it
resuming, the idle dim and the cover holding it still.

ipad-lid: the devices found in a fake sysfs, the daemon through its mock FIFO
and through packed input_event records, with hyprctl, the lock, ipad-sleep and
`passwd -S` replaced by scripts that log: nothing happens until it is switched
on, lock only with a password, display off and back, brightness restored,
suspend only when asked and only after the delay, polarity inversion, a start
with the cover already closed, and switching off with the cover closed."""
import importlib.machinery
import importlib.util
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "rootfs", "files", "usr", "local", "bin")
ALS = os.path.join(BIN, "ipad-als-brightness")
LID = os.path.join(BIN, "ipad-lid")


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


a = load("ipad_als_brightness", ALS)
fails = 0


def check(name, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(f"{'ok ' if ok else 'FAIL'} {name}: {got}" + ("" if ok else f" (want {want})"))


def near(name, got, want, tol=1):
    global fails
    ok = got is not None and abs(got - want) <= tol
    fails += not ok
    print(f"{'ok ' if ok else 'FAIL'} {name}: {got}" + ("" if ok else f" (want {want} +-{tol})"))


# --- the policy, no processes -----------------------------------------------------
pts = a.parse_curve(a.DEFAULT_CURVE)
check("curve: below the first point", a.curve_pct(pts, 0), 10)
check("curve: on a point", round(a.curve_pct(pts, 80), 3), 50)
check("curve: above the last point", a.curve_pct(pts, 50000), 100)
mid = a.curve_pct(pts, 20 * 3)  # between 20 (30 %) and 80 (50 %), geometrically about the middle
check("curve: between points is between them", 30 < mid < 50, True)
check("curve: monotonic", all(a.curve_pct(pts, x) <= a.curve_pct(pts, x * 1.3 + 0.1) for x in [0, 1, 4, 10, 40, 100, 300, 900, 2000]), True)
try:
    a.parse_curve("5:10")
    check("curve: one point rejected", "accepted", "rejected")
except ValueError:
    check("curve: one point rejected", "rejected", "rejected")

conf = a.load_conf()
conf["smooth_secs"] = 3.0
sm = a.Smoother(3.0)
for t in range(5):
    sm.feed(100.0, float(t))
near("smoother: steady", sm.lux, 100.0, 0.5)
sm.feed(5000.0, 5.0)  # one spike among five
near("smoother: a single spike is rejected by the median", sm.lux, 100.0, 0.5)
for t in range(6, 40):
    sm.feed(1000.0, float(t))
near("smoother: settles on a step", sm.lux, 1000.0, 5.0)

pol = a.Policy(conf)
sm2 = a.Smoother(0.0)
sm2.feed(100.0, 0.0)
goal = pol.decide(sm2, 0.0, 10.0)
check("policy: first decision moves at once", goal is not None and abs(goal - pol.goal(100.0)) < 1e-6, True)
sm2.feed(110.0, 1.0)
sm2.feed(110.0, 1.1)
sm2.feed(110.0, 1.2)
check("policy: 10 % more light stays", pol.decide(sm2, 5.0, goal), None)
for t in range(5):
    sm2.feed(300.0, 6.0 + t)
g2 = pol.decide(sm2, 12.0, goal)
check("policy: 3x the light moves", g2 is not None and g2 > goal, True)
for t in range(5):
    sm2.feed(100.0, 13.0 + t)
check("policy: ...but not again within min_interval", pol.decide(sm2, 12.5, g2), None)
check("policy: ...and goes back after it", pol.decide(sm2, 20.0, g2) is not None, True)
check("policy: goal never below min_pct", a.Policy(dict(conf, min_pct=20.0)).goal(0.0), 20.0)

# --- the daemons ----------------------------------------------------------------------
tmp = tempfile.mkdtemp(prefix="als-lid-test-")
try:
    conf_home = os.path.join(tmp, "config")
    state_home = os.path.join(tmp, "state")
    runtime = os.path.join(tmp, "run")
    bl_root = os.path.join(tmp, "backlight")
    bin_dir = os.path.join(tmp, "bin")
    for d in (os.path.join(conf_home, "linux-a9"), state_home, runtime, os.path.join(bl_root, "lcd"), bin_dir):
        os.makedirs(d)
    bl = os.path.join(bl_root, "lcd")
    MAX = 1000
    open(os.path.join(bl, "max_brightness"), "w").write(f"{MAX}\n")
    calls = os.path.join(tmp, "calls.log")
    open(calls, "w").close()

    def script(name, body):
        path = os.path.join(bin_dir, name)
        with open(path, "w") as f:
            f.write(f"#!{sys.executable}\n" + body)
        os.chmod(path, 0o755)
        return path

    # brightnessctl: -q -d lcd set N | -s set 10% | -r
    brightnessctl = script("brightnessctl", f'''
import os, sys
bl = {bl!r}
args = sys.argv[1:]
cur = int(open(bl + "/brightness").read())
if "-r" in args:
    try:
        cur = int(open(bl + "/saved").read())
    except OSError:
        pass
    new = cur
elif "set" in args:
    val = args[args.index("set") + 1]
    if "-s" in args:
        open(bl + "/saved", "w").write(str(cur))
    new = int(round(int(val[:-1]) * {MAX} / 100)) if val.endswith("%") else int(val)
else:
    sys.exit(2)
open(bl + "/brightness", "w").write(str(new))
open({calls!r}, "a").write("brightnessctl " + " ".join(args) + "\\n")
''')
    hyprctl = script("hyprctl", f'''
import sys
open({calls!r}, "a").write("hyprctl " + " ".join(sys.argv[1:]) + "\\n")
''')
    sleeper = script("ipad-sleep", f'''
open({calls!r}, "a").write("ipad-sleep\\n")
''')
    locker = script("lockit", f'''
open({calls!r}, "a").write("lock\\n")
''')
    passwd_p = script("passwd-p", 'print("user P 2026-01-01 0 99999 7 -1")\n')
    passwd_np = script("passwd-np", 'print("user NP 2026-01-01 0 99999 7 -1")\n')

    env0 = dict(
        os.environ, XDG_CONFIG_HOME=conf_home, XDG_STATE_HOME=state_home, XDG_RUNTIME_DIR=runtime,
        IPAD_ALS_BRIGHTNESSCTL=brightnessctl, IPAD_ALS_BACKLIGHT_ROOT=bl_root, IPAD_LID_HYPRCTL=hyprctl,
        IPAD_LID_SLEEP=sleeper, IPAD_LID_PASSWD_STATUS=passwd_np, PYTHONDONTWRITEBYTECODE="1",
    )
    env0.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
    env0.pop("WAYLAND_DISPLAY", None)

    def set_bl(raw):
        open(os.path.join(bl, "brightness"), "w").write(str(raw))

    def get_bl():
        return int(open(os.path.join(bl, "brightness")).read())

    def calllog():
        return open(calls).read().splitlines()

    def clear_calls():
        open(calls, "w").close()

    def run_cmd(prog, *args, env=None):
        return subprocess.run([sys.executable, "-B", prog, *args], env=env or env0, capture_output=True, text=True, timeout=30)

    def start(prog, *args, env=None):
        return subprocess.Popen([sys.executable, "-B", prog, *args], env=env or env0, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)

    def stop(d):
        if d.poll() is None:
            d.send_signal(signal.SIGTERM)
        try:
            out, _ = d.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            d.kill()
            out, _ = d.communicate()
        return out

    def wait_for(fn, timeout=4.0, step=0.05):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if fn():
                return True
            time.sleep(step)
        return fn()

    def stays(fn, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if not fn():
                return False
            time.sleep(0.05)
        return True

    def feed(path, value, seconds, hz=10.0):
        """Write `value` (a lux number) into the FIFO at `hz` for `seconds`."""
        fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            os.write(fd, f"{value}\n".encode())
            time.sleep(1.0 / hz)
        os.close(fd)

    def state(key):
        try:
            for line in open(os.path.join(state_home, "linux-a9", "display")):
                if line.startswith(key + "="):
                    return line.strip().split("=", 1)[1]
        except OSError:
            pass
        return None

    # ===== ipad-als-brightness =====================================================
    with open(os.path.join(conf_home, "linux-a9", "als-brightness.conf"), "w") as f:
        f.write("smooth_secs = 0\npoll_hz = 10\nmin_interval = 0.3\nramp_pct_per_s = 100000\noverride_secs = 1.5\nbogus = 1\n")

    def want_raw(lux):
        return int(round(a.curve_pct(pts, lux) * MAX / 100))

    fifo = os.path.join(tmp, "lux")
    set_bl(500)
    d = start(ALS, "--mock", fifo)
    time.sleep(0.5)
    feed(fifo, 500, 1.5)
    check("als: off by default: brightness untouched", get_bl(), 500)
    check("als: enabled says off", run_cmd(ALS, "enabled").returncode, 1)
    r = run_cmd(ALS, "on")
    check("als: `on` switches it", (r.returncode, state("auto_brightness")), (0, "on"))
    check("als: enabled says on", run_cmd(ALS, "enabled").returncode, 0)
    feed(fifo, 500, 1.2)
    near("als: on: follows 500 lux", get_bl(), want_raw(500), 2)
    first = get_bl()
    feed(fifo, 520, 1.5)
    check("als: 4 % more light: stays", get_bl(), first)
    feed(fifo, 5, 1.5)
    near("als: 5 lux: dim", get_bl(), want_raw(5), 2)
    dark = get_bl()
    # a hand over the sensor for a moment
    fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
    for v in (5, 5, 3000, 5, 5, 5, 5):
        os.write(fd, f"{v}\n".encode())
        time.sleep(0.1)
    os.close(fd)
    time.sleep(0.6)
    check("als: a one-reading spike is ignored", get_bl(), dark)

    # a hand on the brightness key
    set_bl(300)
    clear_calls()
    feed(fifo, 700, 1.0)
    check("als: manual change: paused (no write for a while)", [c for c in calllog() if c.startswith("brightnessctl")], [])
    check("als: manual change: brightness is the user's", get_bl(), 300)
    feed(fifo, 700, 2.0)
    near("als: ...and it resumes after override_secs", get_bl(), want_raw(700), 2)

    # idle dim (hypridle): dim holds, undim releases
    before = get_bl()
    r = run_cmd(ALS, "dim")
    check("als: dim", (r.returncode, get_bl()), (0, 100))
    clear_calls()
    feed(fifo, 20, 1.5)
    check("als: held while dimmed", get_bl(), 100)
    r = run_cmd(ALS, "undim")
    check("als: undim restores", (r.returncode, get_bl()), (0, before))
    check("als: dim flag gone", os.path.exists(os.path.join(runtime, "linux-a9", "dimmed")), False)
    feed(fifo, 20, 2.5)
    near("als: ...and follows again after the settle time", get_bl(), want_raw(20), 2)

    # the cover: ipad-lid's flag holds it still (with a live pid)
    holder = subprocess.Popen(["sleep", "30"])
    os.makedirs(os.path.join(runtime, "linux-a9"), exist_ok=True)
    flag = os.path.join(runtime, "linux-a9", "lid-closed")
    open(flag, "w").write(f"{holder.pid}\nlcd 400\n")
    set_bl(0)
    feed(fifo, 700, 1.5)
    check("als: held while the cover is closed (screen stays off)", get_bl(), 0)
    os.unlink(flag)
    set_bl(400)
    feed(fifo, 700, 3.0)
    near("als: cover open: follows again, no manual-change pause", get_bl(), want_raw(700), 2)
    out = stop(d)
    check("als: the manual change was logged once", out.count("changed by hand"), 1)
    check("als: bad conf key reported", "unknown key bogus" in out, True)
    check("als: status file removed on exit", os.path.exists(os.path.join(runtime, "ipad-als-brightness.json")), False)
    holder.kill()
    holder.wait()
    # a dead pid in the flag doesn't hold
    open(flag, "w").write("999999\n")
    os.environ["XDG_RUNTIME_DIR"] = runtime
    check("als: stale cover flag: no hold", "lid" in a.holds(), False)
    os.unlink(flag)
    # ipad-screen's blank (idle or Power without sleep) holds, at any age
    blanked = os.path.join(runtime, "linux-a9", "blanked")
    open(blanked, "w").write("123.4 lcd 400\n")
    os.utime(blanked, (time.time() - 3 * 3600,) * 2)
    check("als: screen blanked (even for hours): hold", "blanked" in a.holds(), True)
    os.unlink(blanked)
    check("als: unblanked: no hold", "blanked" in a.holds(), False)

    # the switch in the bar and menu: toggle
    run_cmd(ALS, "off")
    check("als: off", state("auto_brightness"), "off")
    run_cmd(ALS, "toggle")
    check("als: toggle", state("auto_brightness"), "on")
    run_cmd(ALS, "off")
    # other keys of the state file stay
    open(os.path.join(state_home, "linux-a9", "display"), "a").write("orientation=landscape\n")
    run_cmd(ALS, "on")
    check("als: state file keeps other keys", state("orientation"), "landscape")

    # sysfs source
    iio = os.path.join(tmp, "iio")
    os.makedirs(os.path.join(iio, "iio:device0"))
    lux_file = os.path.join(iio, "iio:device0", "in_illuminance_input")
    open(lux_file, "w").write("300\n")
    set_bl(500)
    d = start(ALS, "--mock-iio", iio)
    time.sleep(2.5)
    near("als: iio source follows the sysfs lux", get_bl(), want_raw(300), 2)
    open(lux_file, "w").write("10\n")
    time.sleep(2.5)
    near("als: iio source follows a change", get_bl(), want_raw(10), 2)
    stop(d)

    # iio-sensor-proxy path: a fake monitor-sensor
    monitor = script("monitor-sensor", '''
import sys, time
print("    Waiting for iio-sensor-proxy to appear", flush=True)
print("+++ iio-sensor-proxy appeared", flush=True)
print("=== Has ambient light sensor (value: 100.000000, unit: lux)", flush=True)
time.sleep(1.5)
print("Light changed: 4.000000 (vendor)", flush=True)
print("    Light changed: 900.000000 (lux)", flush=True)
time.sleep(60)
''')
    set_bl(500)
    env = dict(env0, IPAD_ALS_MONITOR_CMD=monitor)
    d = start(ALS, "--source", "proxy", env=env)
    time.sleep(1.0)
    near("als: proxy source: first value (100 lux) applied", get_bl(), want_raw(100), 2)
    time.sleep(3.5)
    near("als: proxy source: changed value, vendor units ignored", get_bl(), want_raw(900), 2)
    out = stop(d)
    check("als: vendor units reported", "not lux" in out, True)

    # a dry-run probe changes nothing and says what it would do
    run_cmd(ALS, "off")
    set_bl(500)
    open(lux_file, "w").write("600\n")
    r = run_cmd(ALS, "--probe", "2.5", "--mock-iio", iio)
    check("als: probe leaves the backlight alone", get_bl(), 500)
    check("als: probe says what it would do", "would set brightness" in r.stderr + r.stdout, True)

    # ===== ipad-lid =================================================================
    clear_calls()
    with open(os.path.join(conf_home, "linux-a9", "lid.conf"), "w") as f:
        f.write(f"settle = 0.1\nsuspend_delay = 0.8\nlock_cmd = {locker}\nbogus = 1\n")
    check("lid: no device: --available fails", run_cmd(LID, "--available").returncode, 1)

    sysfs = os.path.join(tmp, "sysfs-input")
    devdir = os.path.join(tmp, "dev-input")
    os.makedirs(devdir)
    for n, name, sw in ((0, "Power Button", "0"), (5, "iPad Smart Cover", "1"), (6, "iPad Smart Cover 2", "10000")):
        base = os.path.join(sysfs, f"input{n}")
        os.makedirs(os.path.join(base, "capabilities"))
        os.makedirs(os.path.join(base, f"event{n}"))
        open(os.path.join(base, "name"), "w").write(name + "\n")
        open(os.path.join(base, "capabilities", "sw"), "w").write(sw + "\n")
    env_dev = dict(env0, IPAD_LID_SYSFS=sysfs, IPAD_LID_DEVDIR=devdir)
    check("lid: finds the cover devices", run_cmd(LID, "--available", env=env_dev).returncode, 0)
    r = run_cmd(LID, "--list", env=env_dev)
    check("lid: --list names both halls and skips the power button",
          ("iPad Smart Cover" in r.stdout, "iPad Smart Cover 2" in r.stdout, "Power Button" in r.stdout), (True, True, False))

    mock = os.path.join(tmp, "lid")

    def cover(*lines):
        fd = os.open(mock, os.O_RDWR | os.O_NONBLOCK)
        for ln in lines:
            os.write(fd, (ln + "\n").encode())
            time.sleep(0.05)
        os.close(fd)

    def status():
        try:
            return json.load(open(os.path.join(runtime, "ipad-lid.json")))
        except (OSError, ValueError):
            return {}

    # (1) off by default: watches only
    set_bl(600)
    d = start(LID, "--mock", mock)
    time.sleep(0.5)
    cover("open")
    time.sleep(0.4)
    cover("closed")
    check("lid: sees the cover close", wait_for(lambda: status().get("cover") == "closed"), True)
    time.sleep(0.5)
    check("lid: actions off by default: nothing done", (calllog(), get_bl()), ([], 600))
    check("lid: no flag while off", os.path.exists(flag), False)
    cover("open")
    # (2) switched on, no password: no lock, display off, brightness back
    r = run_cmd(LID, "on")
    check("lid: `on`", (r.returncode, state("lid_actions")), (0, "on"))
    cover("closed")
    check("lid: closed: display off (dpms disable + backlight 0)",
          wait_for(lambda: get_bl() == 0 and any("dpms" in c and "disable" in c for c in calllog())), True)
    check("lid: closed: no password -> no lock", "lock" in calllog(), False)
    check("lid: closed: flag for ipad-als-brightness", os.path.exists(flag), True)
    check("lid: flag names the daemon and the saved backlight", open(flag).read().split(), [str(d.pid), "lcd", "600"])
    cover("open")
    check("lid: open: backlight back, dpms enable, flag gone",
          wait_for(lambda: get_bl() == 600 and any("dpms" in c and "enable" in c for c in calllog()) and not os.path.exists(flag)), True)
    # (3) with a password: lock
    stop(d)
    clear_calls()
    env_p = dict(env0, IPAD_LID_PASSWD_STATUS=passwd_p)
    d = start(LID, "--mock", mock, env=env_p)
    time.sleep(0.4)
    cover("open")
    time.sleep(0.3)
    cover("closed")
    check("lid: with a password: lock runs before the screen goes",
          wait_for(lambda: calllog()[:1] == ["lock"] and get_bl() == 0), True)
    cover("open")
    wait_for(lambda: get_bl() == 600)
    check("lid: open: brightness back", get_bl(), 600)
    # (4) flapping: a short close under the settle time does nothing
    clear_calls()
    cover("closed")
    time.sleep(0.02)
    cover("open")
    time.sleep(0.6)
    check("lid: a flicker shorter than `settle` does nothing", calllog(), [])
    # (5) suspend is opt-in and waits
    cover("closed")
    wait_for(lambda: get_bl() == 0)
    time.sleep(1.2)
    check("lid: suspend off: closed for longer than the delay: no suspend", "ipad-sleep" in calllog(), False)
    cover("open")
    wait_for(lambda: get_bl() == 600)
    r = run_cmd(LID, "suspend", "on")
    check("lid: `suspend on`", (r.returncode, state("lid_suspend")), (0, "on"))
    clear_calls()
    cover("closed")
    time.sleep(0.4)
    check("lid: suspend on: not before the delay", "ipad-sleep" in calllog(), False)
    check("lid: suspend on: after the delay, once", wait_for(lambda: calllog().count("ipad-sleep") == 1), True)
    time.sleep(0.8)
    check("lid: ...and only once", calllog().count("ipad-sleep"), 1)
    cover("open")
    wait_for(lambda: get_bl() == 600)
    clear_calls()
    cover("closed")
    time.sleep(0.2)
    cover("open")
    time.sleep(1.2)
    check("lid: reopened before the delay: no suspend", "ipad-sleep" in calllog(), False)
    run_cmd(LID, "suspend", "off")
    # (6) the second hall is shown but ignored by default
    clear_calls()
    cover("cover2 1")
    time.sleep(0.5)
    check("lid: second hall ignored by default", (calllog(), status().get("cover2"), status().get("cover")), ([], 1, "open"))
    cover("cover2 0")
    # (7) switched off while the cover is closed: the screen comes back
    cover("closed")
    wait_for(lambda: get_bl() == 0)
    run_cmd(LID, "off")
    check("lid: switched off while closed: backlight back", wait_for(lambda: get_bl() == 600), True)
    cover("open")
    stop(d)

    # (8) invert and cover2 = closed from the conf
    with open(os.path.join(conf_home, "linux-a9", "lid.conf"), "w") as f:
        f.write(f"settle = 0.1\nlock_cmd = {locker}\ninvert = on\ncover2 = closed\n")
    run_cmd(LID, "on")
    clear_calls()
    set_bl(600)
    d = start(LID, "--mock", mock)
    time.sleep(0.4)
    cover("lid 1")  # inverted: the switch says "closed" while the cover is open
    time.sleep(0.5)
    check("lid: invert: SW_LID=1 means open", (status().get("cover"), get_bl()), ("open", 600))
    cover("lid 0")
    check("lid: invert: SW_LID=0 means closed", wait_for(lambda: get_bl() == 0), True)
    cover("lid 1")
    wait_for(lambda: get_bl() == 600)
    stop(d)

    # (9) packed input_event records, the way the kernel writes them
    with open(os.path.join(conf_home, "linux-a9", "lid.conf"), "w") as f:
        f.write("settle = 0.1\n")
    ev = struct.Struct("llHHi")
    pk = os.path.join(tmp, "packed")
    clear_calls()
    d = start(LID, "--mock-evdev", pk)
    time.sleep(0.5)
    fd = os.open(pk, os.O_RDWR | os.O_NONBLOCK)
    os.write(fd, ev.pack(1, 0, 5, 0, 1) + ev.pack(1, 0, 0, 0, 0))  # EV_SW SW_LID 1, then a SYN
    check("lid: evdev record: SW_LID=1 is closed", wait_for(lambda: get_bl() == 0), True)
    os.write(fd, ev.pack(2, 0, 1, 30, 1) + ev.pack(2, 0, 5, 0, 0))  # a key (ignored), then SW_LID 0
    check("lid: evdev record: SW_LID=0 is open, keys ignored", wait_for(lambda: get_bl() == 600), True)
    os.close(fd)
    stop(d)

    # (10) starting with the cover already closed (a daemon restart), then a restart while blanked
    clear_calls()
    with open(os.path.join(tmp, "lid-start"), "w") as f:
        f.write("closed\n")
    d = start(LID, "--mock", os.path.join(tmp, "lid-start"))
    check("lid: starts with the cover closed: acts", wait_for(lambda: get_bl() == 0), True)
    out = stop(d)
    check("lid: stopping the daemon puts the screen back", get_bl(), 600)
    check("lid: flag removed on stop", os.path.exists(flag), False)
    # a flag left by a daemon that died: the next one with the cover open restores it
    open(flag, "w").write("999999\nlcd 450\n")
    set_bl(0)
    with open(os.path.join(tmp, "lid-start"), "w") as f:
        f.write("open\n")
    d = start(LID, "--mock", os.path.join(tmp, "lid-start"))
    check("lid: starts open over a stale blank: screen restored", wait_for(lambda: get_bl() == 450), True)
    stop(d)

    # (11) no cover in the kernel: the daemon exits cleanly, the unit's condition says so first
    r = run_cmd(LID, env=dict(env0, IPAD_LID_SYSFS=os.path.join(tmp, "nothing")))
    check("lid: no switch device: exits 0", r.returncode, 0)

    # (12) the real reader: a FIFO standing in for /dev/input/event5
    os.mkfifo(os.path.join(devdir, "event5"))
    os.mkfifo(os.path.join(devdir, "event6"))
    with open(os.path.join(conf_home, "linux-a9", "lid.conf"), "w") as f:
        f.write("settle = 0.1\n")
    run_cmd(LID, "on")
    set_bl(600)
    clear_calls()
    d = start(LID, env=env_dev)
    time.sleep(0.6)
    fd = os.open(os.path.join(devdir, "event5"), os.O_RDWR | os.O_NONBLOCK)
    os.write(fd, ev.pack(3, 0, 5, 0, 1))
    check("lid: the daemon reads the device node it found", wait_for(lambda: get_bl() == 0), True)
    os.write(fd, ev.pack(4, 0, 5, 0, 0))
    check("lid: ...and the cover opening", wait_for(lambda: get_bl() == 600), True)
    os.close(fd)
    out = stop(d)
    check("lid: the second hall's device was opened too", "iPad Smart Cover 2" in out, True)
    r = run_cmd(LID, "--watch", "1", env=env_dev)
    check("lid: --watch runs", r.returncode, 0)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("FAILED" if fails else "all ok")
sys.exit(1 if fails else 0)
