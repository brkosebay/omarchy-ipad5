#!/usr/bin/env python3
"""Write the speaker test WAVs (16-bit stereo PCM) into OUTDIR.

  tone-1k-48k.wav     1 kHz sine, -12 dBFS, 3 s, both channels, 48 kHz
  tone-1k-44k1.wav    same at 44.1 kHz (the rate iOS last used)
  left-right-48k.wav  440 Hz left only (1.5 s), then 660 Hz right only (1.5 s)
  sweep-48k.wav       200 Hz -> 4 kHz log sweep, -18 dBFS, 4 s

Fades of 50 ms at both ends avoid clicks. No dependencies.
"""
import math
import struct
import sys
from pathlib import Path


def wav(path, rate, frames):
    data = b"".join(struct.pack("<hh", l, r) for l, r in frames)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16)
    hdr += b"data" + struct.pack("<I", len(data))
    path.write_bytes(hdr + data)


def env(i, n, rate):
    f = int(0.05 * rate)
    return min(1.0, i / f, (n - i) / f)


def tone(rate, hz, secs, dbfs, left=True, right=True):
    n, a = int(rate * secs), 10 ** (dbfs / 20) * 32767
    for i in range(n):
        v = int(a * env(i, n, rate) * math.sin(2 * math.pi * hz * i / rate))
        yield (v if left else 0, v if right else 0)


def sweep(rate, f0, f1, secs, dbfs):
    n, a, ph = int(rate * secs), 10 ** (dbfs / 20) * 32767, 0.0
    for i in range(n):
        f = f0 * (f1 / f0) ** (i / n)
        ph += 2 * math.pi * f / rate
        v = int(a * env(i, n, rate) * math.sin(ph))
        yield (v, v)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    wav(out / "tone-1k-48k.wav", 48000, tone(48000, 1000, 3, -12))
    wav(out / "tone-1k-44k1.wav", 44100, tone(44100, 1000, 3, -12))
    lr = list(tone(48000, 440, 1.5, -12, right=False)) + \
        list(tone(48000, 660, 1.5, -12, left=False))
    wav(out / "left-right-48k.wav", 48000, lr)
    wav(out / "sweep-48k.wav", 48000, sweep(48000, 200, 4000, 4, -18))
    print("wrote", ", ".join(sorted(p.name for p in out.glob("*.wav"))))


if __name__ == "__main__":
    main()
