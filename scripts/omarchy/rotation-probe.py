#!/usr/bin/env python3
"""Bounded, unprivileged rotation observer; run ONLY by the orchestrator.

Emit JSONL with monotonic receipt times, IPC latency, monitor state, appended
Hyprland logs, CPU ticks, and optionally small grim samples. Screenshot samples
observe compositor content, not panel light, and perturb software rendering.
The action starts after one second; it must be an argv after `--` (no shell).
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=7)
    parser.add_argument("--screenshots", type=Path, help="new directory for PPMs; adds render load")
    parser.add_argument("--layers", action="store_true", help="also sample layer geometry/PIDs/alpha (extra IPC)")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not 2 <= args.seconds <= 12 or not command:
        parser.error("need 2 <= --seconds <= 12 and -- COMMAND [ARG ...]")
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", ""))
    signature = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE", "")
    instance = runtime / "hypr" / signature
    if not signature or not (instance / ".socket.sock").is_socket():
        parser.error("export XDG_RUNTIME_DIR and a live HYPRLAND_INSTANCE_SIGNATURE first")
    if args.screenshots:
        args.screenshots.mkdir(parents=True, exist_ok=False)

    start_ns = time.monotonic_ns()
    start = start_ns / 1e9
    lock = threading.Lock()

    def emit(kind, **fields):
        with lock:
            print(json.dumps(dict(event=kind, ms=round((time.monotonic() - start) * 1000, 3), **fields)), flush=True)

    def run(argv, timeout=1):
        try:
            process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        except OSError as error:
            return 127, b"", str(error)
        try:
            out, err = process.communicate(timeout=timeout)
            return process.returncode, out, err.decode(errors="replace")
        except subprocess.TimeoutExpired:
            # Only this observer's subprocess group, never a process-name kill.
            # A timed-out shell action must not leave its hyprctl holding pipes.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
            return 124, b"", "timeout"

    def cpu():
        result = {}
        for proc in Path("/proc").glob("[0-9]*"):
            try:
                if proc.stat().st_uid != os.getuid():
                    continue
                name = (proc / "comm").read_text().strip()
                if name not in {"Hyprland", "quickshell", "qs"}:
                    continue
                fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
                result[proc.name] = dict(name=name, ticks=int(fields[11]) + int(fields[12]))
            except (OSError, ValueError, IndexError):
                continue
        return result

    # Tail only this run's bytes. Receipt times include the observer's polling
    # delay; native timestamps, when enabled, remain in the text itself.
    logfile = instance / "hyprland.log"
    logstream = logfile.open(errors="replace") if logfile.is_file() else None
    if logstream:
        logstream.seek(0, 2)

    def logs():
        if logstream:
            for line in logstream.readlines():
                emit("log", text=line.rstrip())

    def monitors():
        then = time.monotonic()
        rc, out, err = run(["hyprctl", "-j", "monitors"])
        try:
            value = json.loads(out) if rc == 0 else None
        except ValueError:
            value = None
        emit("monitors", ipc_ms=round((time.monotonic() - then) * 1000, 3), rc=rc, monitors=value, error=err)
        if args.layers:
            rc, out, err = run(["hyprctl", "-j", "layers"])
            try:
                layers = json.loads(out) if rc == 0 else None
            except ValueError:
                layers = None
            emit("layers", rc=rc, layers=layers, error=err)
        return value

    emit("start", wall_ns=time.time_ns(), start_monotonic_ns=start_ns, clock_ticks=os.sysconf("SC_CLK_TCK"), command=command, cpu=cpu(), screenshots=bool(args.screenshots))
    initial = monitors()
    if not isinstance(initial, list) or len(initial) != 1:
        raise SystemExit("probe requires exactly one enabled monitor")
    output = initial[0]["name"]

    action_result = []

    def action():
        time.sleep(max(0, start + 1 - time.monotonic()))
        emit("action_start")
        rc, out, err = run(command, timeout=8)
        action_result.append(rc)
        emit("action_end", rc=rc, stdout=out.decode(errors="replace"), stderr=err)

    worker = threading.Thread(target=action)
    worker.start()
    index = 0
    while time.monotonic() - start < args.seconds:
        logs()
        monitors()
        if args.screenshots:
            then = time.monotonic()
            rc, out, err = run(["grim", "-t", "ppm", "-s", "0.0625", "-o", output, "-"], timeout=1)
            fields = dict(rc=rc, capture_start_ms=round((then - start) * 1000, 3), error=err)
            # grim's P6 output has no comments. Keep every successful sample
            # for inspection; brightness alone cannot distinguish dark artwork.
            header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", out)
            if rc == 0 and header:
                width, height = map(int, header.groups())
                pixels = out[header.end():]
                if len(pixels) == width * height * 3:
                    file = args.screenshots / f"{index:03d}.ppm"
                    file.write_bytes(out)
                    center = ((height // 2) * width + width // 2) * 3
                    black = sum(max(pixels[i:i + 3]) <= 8 for i in range(0, len(pixels), 3))
                    fields.update(file=str(file), mean_rgb=sum(pixels) / len(pixels), width=width, height=height,
                                  center_rgb=list(pixels[center:center + 3]), black_fraction=black / (width * height))
            emit("screenshot", **fields)
        index += 1
        time.sleep(0.2)
    worker.join()  # action subprocess has its own 8-second timeout
    logs()
    monitors()
    emit("end", cpu=cpu())
    if logstream:
        logstream.close()
    return action_result[0] if action_result else 1


if __name__ == "__main__":
    raise SystemExit(main())
