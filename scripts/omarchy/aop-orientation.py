#!/usr/bin/env python3
"""Capture one labelled pose on Linux; derive a proxy-compatible matrix on host.

No device transport: capture is run by capture-aop-orientation.sh over SSH,
only when the orchestrator invokes it. Derive reads saved files only.
"""
import argparse
import itertools
import json
import math
from pathlib import Path
import statistics
import time

G = 9.80665
# Signs from iio-sensor-proxy 3.9 src/orientation.c and tilt_calc().
TARGETS = {
    "screen-up": (0, 0, G), "screen-down": (0, 0, -G),
    "portrait": (0, -G, 0), "portrait-flipped": (0, G, 0),
    "landscape": (-G, 0, 0), "landscape-flipped": (G, 0, 0),
}


def capture(pose, seconds):
    if not 2 <= seconds <= 30:
        raise ValueError("capture duration must be 2..30 seconds")
    devices = [p for p in Path("/sys/bus/iio/devices").glob("iio:device*")
               if (p / "name").read_text().strip() == "apple-a9-aop-accel"]
    if len(devices) != 1:
        raise ValueError(f"expected one streaming AOP accel, found {len(devices)}")
    dev = devices[0]
    samples = []
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        samples.append({"time_ns": time.monotonic_ns(),
                        "raw": [int((dev / f"in_accel_{a}_raw").read_text()) for a in "xyz"]})
        time.sleep(0.1)
    return {"schema": 1, "pose": pose, "name": (dev / "name").read_text().strip(),
            "device": str(dev), "scale": float((dev / "in_accel_scale").read_text()),
            "sampling_frequency": (dev / "in_accel_sampling_frequency").read_text().strip(),
            "kernel": Path("/proc/sys/kernel/osrelease").read_text().strip(),
            "cmdline": Path("/proc/cmdline").read_text().strip(),
            "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "samples": samples}


def derive(directory):
    means, details, boots = {}, {}, set()
    for pose in TARGETS:
        data = json.loads((directory / f"{pose}.json").read_text())
        assert data["pose"] == pose and data["name"] == "apple-a9-aop-accel", pose
        boots.add(data["boot_id"])
        samples, scale = data["samples"], data["scale"]
        assert len(samples) >= 20 and 0 < scale < 1, f"{pose}: incomplete capture/scale"
        assert all(a["time_ns"] < b["time_ns"] for a, b in zip(samples, samples[1:])), pose
        axes = [[s["raw"][i] * scale for s in samples] for i in range(3)]
        mean = [statistics.mean(a) for a in axes]
        sd = [statistics.pstdev(a) for a in axes]
        assert max(sd) < 0.08 * G, f"{pose}: moving; hold still and repeat"
        assert 0.8 * G < math.sqrt(sum(v*v for v in mean)) < 1.2 * G, f"{pose}: bad gravity magnitude"
        means[pose] = mean
        details[pose] = {"mean_ms2": mean, "stddev_ms2": sd, "samples": len(samples)}
    assert len(boots) == 1, "capture all six poses during the same boot"
    candidates = []
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product((-1, 1), repeat=3):
            matrix = [[int(perm[r] == c) * signs[r] for c in range(3)] for r in range(3)]
            errors = [math.dist([signs[r] * v[perm[r]] for r in range(3)], TARGETS[p])
                      for p, v in means.items()]
            candidates.append((sum(e*e for e in errors), max(errors), matrix))
    candidates.sort()
    score, worst, matrix = candidates[0]
    assert worst < 0.2 * G, f"poses do not fit a signed axis permutation (worst {worst:.2f} m/s²)"
    assert candidates[1][0] - score > G*G, "ambiguous axes; repeat labelled poses"
    return {"schema": 1, "validated": True, "name": "apple-a9-aop-accel",
            "frame": "iio-sensor-proxy-3.9", "matrix": matrix,
            "matrix_string": ";".join(",".join(map(str, row)) for row in matrix),
            "worst_error_ms2": worst, "poses": details, "boot_id": boots.pop(),
            "source": str(directory.resolve()),
            "note": "Six static poses fit; buffer, D-Bus and physical touch tests still required."}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    cp = sub.add_parser("capture")
    cp.add_argument("pose", choices=TARGETS)
    cp.add_argument("--seconds", type=float, default=3)
    dp = sub.add_parser("derive")
    dp.add_argument("directory", type=Path)
    dp.add_argument("--output", type=Path)
    args = ap.parse_args()
    result = capture(args.pose, args.seconds) if args.command == "capture" else derive(args.directory)
    text = json.dumps(result, indent=2) + "\n"
    if args.command == "derive" and args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
