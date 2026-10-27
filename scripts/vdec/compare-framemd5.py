#!/usr/bin/env python3
"""Compare the device's per-frame MD5s with ffmpeg's software decode.

  python3 -I scripts/vdec/compare-framemd5.py GST.sum REF.framemd5

GST.sum: what `... ! v4l2slh264dec ! checksumsink hash=md5` printed on the
device, one "<pts> <md5>" line per frame; or (round 7) the device's
`ffmpeg -hwaccel v4l2request ... -pix_fmt nv12 -f framemd5` output, whose
lines also end in the MD5 ('#' header lines are skipped: their last field
is not 32 characters).  REF.framemd5: one MD5 per line
in display order, from scripts/vdec/make-h264-streams.sh --clips (ffmpeg's
software decode, tight NV12).  Both hash the same bytes: GStreamer copies
each frame into tight NV12 for a sink without video-meta support.

Prints the frame counts, the first differing frame and the number of
equal frames; exit 0 only when every frame matches.
"""
import pathlib
import sys


def main():
    gst = [l.split()[-1].lower() for l in pathlib.Path(sys.argv[1]).read_text().splitlines()
           if l.strip() and len(l.split()[-1]) == 32]
    ref = [l.strip().lower() for l in pathlib.Path(sys.argv[2]).read_text().splitlines() if l.strip()]
    same = sum(a == b for a, b in zip(gst, ref))
    first = next((i for i, (a, b) in enumerate(zip(gst, ref)) if a != b), None)
    print(f'frames: device {len(gst)}, ffmpeg {len(ref)}; identical {same}'
          + (f'; first difference at frame {first} (display order)' if first is not None else ''))
    ok = len(gst) == len(ref) and same == len(ref)
    print('framemd5: ' + ('PASS (bit-exact)' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
