#!/usr/bin/env python3
"""Orchestrator-only, opt-in rotation A/B; no default session config changes.

Profiles: damage = whole-output damage; burst = damage + continuous frames;
hold-shell = SIGSTOP only the PID owning Omarchy layers, retaining its buffers.
Wrap a rotation argv for three seconds. Restore on exit/signals; an independent
six-second watchdog also restores if this wrapper is killed or SSH disappears.
Example: python3 rotation-control-ab.py burst -- ipad-rotate portrait
Never use hold-shell on a locked session. Nothing here stops Hyprland or QEMU.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import fcntl


def command(argv, timeout=2):
    process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.communicate()
        raise RuntimeError(f"command timeout: {argv[0]}") from None
    if process.returncode:
        raise RuntimeError(f"rc={process.returncode}: {argv[0]}: {err.decode(errors='replace')}")
    return out.decode()


def identity(pid):
    proc = Path(f"/proc/{pid}")
    fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
    return dict(pid=pid, uid=proc.stat().st_uid, start=fields[19], state=fields[0])


def restore(lease):
    if not lease.exists():
        return
    saved = json.loads(lease.read_text())
    if saved["profile"] == "hold-shell":
        try:
            current = identity(saved["shell"]["pid"])
            if all(current[k] == saved["shell"][k] for k in ("pid", "uid", "start")):
                os.kill(current["pid"], signal.SIGCONT)
        except FileNotFoundError:
            pass
    else:
        # Refuse to target a different/restarted compositor instance.
        if saved["signature"] != os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
            raise RuntimeError("restore instance mismatch")
        command(["hyprctl", "eval", saved["restore_eval"]])
    lease.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lease", type=Path, help="new state file; retained on failed restore for manual recovery")
    parser.add_argument("--restore-file", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--delay", type=float, default=0, help=argparse.SUPPRESS)
    parser.add_argument("profile", nargs="?", choices=("damage", "burst", "hold-shell"))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.restore_file:
        time.sleep(args.delay)
        restore(args.restore_file)
        return 0
    argv = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not args.profile or not argv:
        parser.error("need PROFILE -- ROTATION_COMMAND [ARG ...]")
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", ""))
    signature = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE", "")
    if not signature or not (runtime / "hypr" / signature / ".socket.sock").is_socket():
        parser.error("export XDG_RUNTIME_DIR and a live HYPRLAND_INSTANCE_SIGNATURE")
    mutex = (runtime / "rotation-control-ab.lock").open("a")
    fcntl.flock(mutex, fcntl.LOCK_EX | fcntl.LOCK_NB)
    saved = dict(profile=args.profile, signature=signature)
    if args.profile == "hold-shell":
        # This command's exit 1 means unlocked, 0 means locked. Fail closed
        # on a missing helper, timeout or other error before sending SIGSTOP.
        check = subprocess.run(["omarchy-hyprland-session-locked"], timeout=2, check=False)
        if check.returncode != 1:
            raise RuntimeError("hold-shell requires a verified unlocked session")
        layers = json.loads(command(["hyprctl", "-j", "layers"]))
        pids = {int(layer["pid"]) for mon in layers.values() for level in mon["levels"].values()
                for layer in level if layer["namespace"].startswith("omarchy-")}
        if len(pids) != 1:
            raise RuntimeError(f"expected one Omarchy layer owner, found {sorted(pids)}")
        shell = identity(pids.pop())
        name = Path(f'/proc/{shell["pid"]}/comm').read_text().strip()
        if shell["uid"] != os.getuid() or shell["state"] in {"T", "t"} or name not in {"quickshell", "qs"}:
            raise RuntimeError("unexpected/stopped Omarchy process; refusing SIGSTOP")
        saved["shell"] = shell
    else:
        old = json.loads(command(["hyprctl", "-j", "getoption", "debug:damage_tracking"]))["int"]
        saved["restore_eval"] = f"hl.config({{ debug = {{ damage_tracking = {int(old)}"
        if args.profile == "burst":
            vfr = json.loads(command(["hyprctl", "-j", "getoption", "debug:vfr"]))["bool"]
            saved["restore_eval"] += f", vfr = {str(vfr).lower()}"
        saved["restore_eval"] += " } })"
    if args.lease:
        fd = os.open(args.lease, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        lease = args.lease
    else:
        fd, path = tempfile.mkstemp(prefix="rotation-control-", suffix=".json", dir=runtime)
        lease = Path(path)
    with os.fdopen(fd, "w") as file:
        json.dump(saved, file)

    def stop(signum, _frame):
        raise SystemExit(128 + signum)

    for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, stop)
    guard = None
    begin = time.monotonic()

    def emit(event, **fields):
        print(json.dumps(dict(event=event, ms=round((time.monotonic() - begin) * 1000, 3), monotonic_ns=time.monotonic_ns(), **fields)), flush=True)

    try:
        # Separate session + separate pipes: parent/SSH termination cannot
        # strand a stopped shell, and cannot keep action stdout pipes open.
        with lease.with_suffix(".watchdog.log").open("a") as log:
            guard = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--restore-file", str(lease), "--delay", "6"],
                                     stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        if args.profile == "hold-shell":
            current = identity(saved["shell"]["pid"])
            if any(current[k] != saved["shell"][k] for k in ("pid", "uid", "start")):
                raise RuntimeError("Omarchy process changed before SIGSTOP")
            os.kill(saved["shell"]["pid"], signal.SIGSTOP)
        else:
            fields = "damage_tracking = 0" + (", vfr = false" if args.profile == "burst" else "")
            command(["hyprctl", "eval", "hl.config({ debug = { " + fields + " } })"])
        emit("profile_applied", profile=args.profile, lease=str(lease), shell=saved.get("shell"))
        out = command(argv, timeout=5)
        emit("rotation_return", stdout=out)
        time.sleep(max(0, begin + 3 - time.monotonic()))
        return 0
    finally:
        # Leave the watchdog armed and the lease intact if restoration fails.
        restore(lease)
        emit("profile_restored")
        if guard and guard.poll() is None:
            guard.terminate()  # exact child PID created here
            guard.wait(timeout=2)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print(f"rotation-control-ab: {error}", file=sys.stderr)
        raise SystemExit(1)
