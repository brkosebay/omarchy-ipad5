#!/usr/bin/env python3
"""Offline check of ipad-screen and ipad-sleep (runs on the Mac, no Hyprland,
no backlight, no systemd):  python3 scripts/omarchy/qemu/test-screen-sleep.py

brightnessctl, hyprctl and systemctl are replaced by scripts that log and
write a fake backlight tree. Checked: blank and unblank (DPMS and backlight,
the level remembered across a second `off`), `wake` ignoring a blank younger
than 1.5 s, `on` doing nothing (no DPMS modeset) when nothing was blanked, the
Smart Cover's screen left to ipad-lid, a level of 0 coming back at half; then
ipad-sleep: Power blanks and suspends, Power on a blanked screen only turns it
on, a Power press right after an unblank is dropped, a masked suspend.target
or a refused suspend leaves the screen off (and never unmasks anything), and
idle blanks without suspending unless idle-suspend is on."""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.abspath(os.path.join(HERE, "..", "rootfs", "files", "usr", "local", "bin"))
fails = 0
# Past ipad-screen's 1.5 s grace. On the Mac the scripts fall back to whole
# seconds (no /proc/uptime), so leave a margin.
GAP = 2.1


def check(name, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(f"{'ok ' if ok else 'FAIL'} {name}: {got}" + ("" if ok else f" (want {want})"))


tmp = tempfile.mkdtemp(prefix="screen-sleep-")
fake = os.path.join(tmp, "fakebin")
bl = os.path.join(tmp, "backlight", "lcd")
runtime = os.path.join(tmp, "run")
state = os.path.join(tmp, "state")
calls = os.path.join(tmp, "calls")
for p in (fake, bl, runtime, state):
    os.makedirs(p)

FAKES = {
    "brightnessctl": f"""#!/bin/sh
echo "brightnessctl $*" >>{calls}
while [ $# -gt 0 ]; do case $1 in -d) shift; dev=$1 ;; set) shift; echo "$1" >"{tmp}/backlight/$dev/brightness" ;; esac; shift; done
""",
    "hyprctl": f"""#!/bin/sh
echo "hyprctl $*" >>{calls}
""",
    # FAKE_SUSPEND: ok (default), masked, refused
    "systemctl": f"""#!/bin/sh
echo "systemctl $*" >>{calls}
case "$1 ${{FAKE_SUSPEND:-ok}}" in
"is-enabled masked") echo masked ;;
is-enabled*) echo static ;;
"suspend refused") echo "Operation inhibited" >&2; exit 1 ;;
"unmask "*|"mask "*) echo "systemctl $*" >>{tmp}/forbidden ;;
esac
exit 0
""",
}
for name, body in FAKES.items():
    path = os.path.join(fake, name)
    with open(path, "w") as f:
        f.write(body)
    os.chmod(path, 0o755)

env = dict(os.environ, PATH=f"{fake}:{BIN}:{os.environ['PATH']}", XDG_RUNTIME_DIR=runtime,
           XDG_STATE_HOME=state, IPAD_ALS_BACKLIGHT_ROOT=os.path.dirname(bl), HYPRLAND_INSTANCE_SIGNATURE="")
env.pop("FAKE_SUSPEND", None)


def run(*args, **extra):
    return subprocess.run(["bash", os.path.join(BIN, args[0]), *args[1:]], env=dict(env, **extra),
                          capture_output=True, text=True)


def get_bl():
    return int(open(os.path.join(bl, "brightness")).read())


def set_bl(v):
    open(os.path.join(bl, "brightness"), "w").write(f"{v}\n")


def calllog():
    try:
        return open(calls).read().splitlines()
    except OSError:
        return []


def clear():
    open(calls, "w").close()


def dpms():
    return [c.split('action = "')[1].split('"')[0] for c in calllog() if c.startswith("hyprctl")]


def suspends():
    return [c for c in calllog() if c == "systemctl suspend"]


flag = os.path.join(runtime, "linux-a9", "blanked")
open(os.path.join(bl, "max_brightness"), "w").write("2047\n")
set_bl(800)

try:
    # --- ipad-screen ---
    check("screen: status at start", run("ipad-screen").stdout.strip(), "on")
    clear()
    r = run("ipad-screen", "off")
    check("screen: off", (r.returncode, get_bl(), dpms()), (0, 0, ["disable"]))
    check("screen: status blanked", run("ipad-screen").stdout.strip(), "blanked")
    run("ipad-screen", "off")
    check("screen: second off keeps the level (not 0)", open(flag).read().split()[1:], ["lcd", "800"])
    clear()
    run("ipad-screen", "wake")
    check("screen: wake right after a blank does nothing", (get_bl(), dpms()), (0, []))
    time.sleep(GAP)
    run("ipad-screen", "wake")
    check("screen: wake later restores", (get_bl(), dpms(), os.path.exists(flag)), (800, ["enable"], False))
    clear()
    run("ipad-screen", "on")
    run("ipad-screen", "wake")
    check("screen: on/wake with nothing blanked: no DPMS call", dpms(), [])

    # the cover closed (ipad-lid's flag with a live pid): its screen stays off
    holder = subprocess.Popen(["sleep", "30"])
    lidflag = os.path.join(runtime, "linux-a9", "lid-closed")
    open(lidflag, "w").write(f"{holder.pid}\nlcd 800\n")
    set_bl(0)
    run("ipad-screen", "off")
    check("screen: off with the cover closed remembers no level", open(flag).read().split()[1:], ["lcd"])
    clear()
    run("ipad-screen", "on")
    check("screen: on with the cover closed leaves it dark", (get_bl(), dpms(), os.path.exists(flag)), (0, [], False))
    holder.kill()
    os.unlink(lidflag)

    # dark with nothing remembered: half
    run("ipad-screen", "off")
    run("ipad-screen", "on")
    check("screen: level 0 comes back at half", get_bl(), 1023)

    # --- ipad-sleep ---
    set_bl(800)
    time.sleep(GAP)  # the unblank just above would drop the next Power press
    clear()
    r = run("ipad-sleep")
    check("sleep: Power blanks and suspends", (r.returncode, get_bl(), suspends()), (0, 0, ["systemctl suspend"]))
    clear()
    r = run("ipad-sleep")
    check("sleep: Power on a blanked screen only turns it on", (r.returncode, get_bl(), suspends()), (0, 800, []))
    clear()
    r = run("ipad-sleep")
    check("sleep: Power right after an unblank is dropped", (get_bl(), dpms(), suspends()), (800, [], []))
    time.sleep(GAP)

    clear()
    r = run("ipad-sleep", FAKE_SUSPEND="masked")
    check("sleep: masked suspend.target: screen off, no suspend", (r.returncode, get_bl(), suspends()), (0, 0, []))
    check("sleep: ...and says why", "masked" in r.stderr, True)
    run("ipad-sleep")
    time.sleep(GAP)

    clear()
    r = run("ipad-sleep", FAKE_SUSPEND="refused")
    check("sleep: refused suspend: screen stays off", (r.returncode, get_bl(), run("ipad-screen").stdout.strip()),
          (0, 0, "blanked"))
    run("ipad-sleep")
    time.sleep(GAP)

    check("sleep: idle-suspend default", run("ipad-sleep", "idle-suspend").stdout.strip(), "off")
    clear()
    r = run("ipad-sleep", "--idle")
    check("sleep: idle blanks, no suspend", (r.returncode, get_bl(), suspends()), (0, 0, []))
    time.sleep(GAP)
    run("ipad-screen", "wake")
    check("sleep: ...touch turns it back on", get_bl(), 800)
    check("sleep: idle-suspend on", run("ipad-sleep", "idle-suspend", "on").stdout.strip(), "on")
    check("sleep: kept in the state file", "idle_suspend=on" in open(os.path.join(state, "linux-a9", "display")).read(), True)
    clear()
    run("ipad-sleep", "--idle")
    check("sleep: idle with idle-suspend on suspends", (get_bl(), suspends()), (0, ["systemctl suspend"]))
    check("sleep: bad argument", run("ipad-sleep", "bogus").returncode, 2)

    # ipad-lid's opt-in suspend runs ipad-sleep with the cover closed: it must
    # sleep even when the screen was already blanked (no Power toggle)
    run("ipad-sleep", "idle-suspend", "off")
    time.sleep(GAP)
    holder = subprocess.Popen(["sleep", "30"])
    open(lidflag, "w").write(f"{holder.pid}\nlcd 800\n")
    run("ipad-screen", "off")
    clear()
    r = run("ipad-sleep")
    check("sleep: cover closed + blanked: suspends, no toggle", (r.returncode, suspends(), dpms()),
          (0, ["systemctl suspend"], ["disable"]))
    holder.kill()
    os.unlink(lidflag)
    check("nothing masked or unmasked", os.path.exists(os.path.join(tmp, "forbidden")), False)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("RESULT:", "all passed" if not fails else f"{fails} failed")
sys.exit(1 if fails else 0)
