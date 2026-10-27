#!/usr/bin/env python3
"""Desktop cost bench for the Omarchy iPad layer, run inside the session's VM
(or on the device) as the desktop user, over SSH.

  desktop-bench.py [LABEL [SCENARIO,...]]   (default: all scenarios)

Fixed workloads, each measured as CPU-seconds (all cores) per process from
/proc, plus wall time where it means something:
  idle      20 s of nothing
  terminal  a foot terminal printing 300 lines at ~40 lines/s: scrolling, so
            the whole window really changes every frame
  typing    one line rewritten in place at ~40 Hz: a few hundred pixels per
            frame, cheap only when damage tracking works (it didn't for foot
            until the ext-background-effect shim, docs/omarchy/hyprland.md)
  switch    12 workspace switches between a terminal and the empty wallpaper
            workspace (full-screen redraws)
  menu      the Omarchy menu opened and closed 6 times (Quickshell)
Lower is better. Absolute numbers on the Mac's QEMU cores are several times
lower than on the A9; compare variants, not machines. Hyprland's config is
Lua, so dispatches use hl.dsp (hyprctl dispatch 'hl.dsp.focus(...)').
Prints one JSON line (scripts/omarchy/qemu/README or docs/omarchy/hyprland.md).
"""
import glob
import json
import os
import subprocess
import sys
import time

HZ = os.sysconf("SC_CLK_TCK")
WATCH = ("Hyprland", "quickshell", ".quickshell-wra", "foot", "Xwayland", "pipewire",
         "wireplumber", "elephant", "walker", "hypridle", "uwsm", "sddm")


def procs():
    out = {}
    for path in glob.glob("/proc/[0-9]*"):
        try:
            comm = open(path + "/comm").read().strip()
            fields = open(path + "/stat").read().rsplit(")", 1)[1].split()
            out[int(path[6:])] = (comm, int(fields[11]) + int(fields[12]))
        except (OSError, IndexError, ValueError):
            pass
    return out


def system_busy():
    vals = list(map(int, open("/proc/stat").readline().split()[1:]))
    return sum(vals) - vals[3] - vals[4]


def measure(fn):
    before, busy0, t0 = procs(), system_busy(), time.time()
    fn()
    after, busy1, t1 = procs(), system_busy(), time.time()
    per = {}
    for pid, (comm, ticks) in after.items():
        prev = before.get(pid)
        delta = ticks - (prev[1] if prev and prev[0] == comm else 0)
        if delta > 0:
            per[comm] = per.get(comm, 0) + delta
    top = {k: round(v / HZ, 2) for k, v in sorted(per.items(), key=lambda kv: -kv[1])[:6]}
    return {"wall_s": round(t1 - t0, 1), "cpu_s": round((busy1 - busy0) / HZ, 2), "top": top}


def hyprctl(*args):
    subprocess.run(["hyprctl", *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def foot(script, marker=None):
    return subprocess.Popen(["foot", "-e", "sh", "-c", script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "run"
    only = set(sys.argv[2].split(",")) if len(sys.argv) > 2 else None
    want = lambda name: only is None or name in only
    result = {"label": label}
    hyprctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
    time.sleep(3)

    if want("idle"):
        result["idle"] = measure(lambda: time.sleep(20))

    marker = "/tmp/desktop-bench.done"
    if os.path.exists(marker):
        os.unlink(marker)
    term = None
    def wait_terminal():
        while not os.path.exists(marker):
            time.sleep(0.2)
    if want("terminal"):
      term = foot("sleep 2; i=0; while [ $i -lt 300 ]; do i=$((i+1)); echo \"line $i: the quick brown fox jumps over the lazy dog\"; sleep 0.025; done; touch %s; sleep 60" % marker)

      time.sleep(1)
      result["terminal"] = measure(wait_terminal)
      term.terminate()

    # One line rewritten in place at ~40 Hz: a few hundred pixels change per
    # frame, so with working damage tracking this costs a fraction of the above.
    if want("typing"):
      if os.path.exists(marker):
          os.unlink(marker)
      term = foot("sleep 2; i=0; while [ $i -lt 300 ]; do i=$((i+1)); printf '\\rtyping %%d' $i; sleep 0.025; done; touch %s; sleep 60" % marker)
      time.sleep(1)
      result["typing"] = measure(wait_terminal)

    def switch():
        for _ in range(6):
            hyprctl("dispatch", 'hl.dsp.focus({ workspace = "2" })')
            time.sleep(0.6)
            hyprctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
            time.sleep(0.6)
    if want("switch"):
        result["switch"] = measure(switch)

    def menu():
        for _ in range(6):
            subprocess.run(["omarchy-menu", "toggle"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.0)
            subprocess.run(["omarchy-menu", "close"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(0.8)
    if want("menu"):
        result["menu"] = measure(menu)

    if term:
        term.terminate()
    hyprctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
    print(json.dumps(result))


if __name__ == "__main__":
    main()
