#!/usr/bin/env bash
# Generate the apple-vxd H.264 test bitstreams (Annex-B, progressive) with
# ffmpeg/libx264 on the Mac, their reference NV12 decodes (ffmpeg's
# software decoder, display order), and scripts/vdec/h264-streams.h: the
# small streams as byte arrays plus a CRC-32 per reference frame, embedded
# in vdec-h264-test.c and the host command-stream harness.
#
# Embedded (all without SEI, so the bytes depend only on libx264's choices;
# B-frame streams use b-pyramid=none and b-adapt=0, so every B picture is a
# non-reference picture and no stream needs adaptive reference marking):
#   grey64   64x64 flat grey, 1 IDR, 1 slice          (round 5)
#   ts176    176x144 testsrc2, IDR + 2 P, 1 slice      (round 5)
#   ts176s4  the same content, 4 slices per picture     (round 5)
#   ts176cb  the same content, Main profile with CABAC   (round 5)
#   hp176    High: CABAC, 8x8 transform, IBBPBBP, spatial direct, 2 refs
#   hp176t   High: CAVLC, 8x8 transform, IBBPBBP, temporal direct
#            (the co-located picture's stored motion is read)
#   hp176q   High: cqm=jvt, the "use default" scaling lists, 8x8, B
#   hp176m   High: an asymmetric custom matrix (cqmfile), 8x8, B: a
#            transposed or zig-zag-ordered upload shows up as a mismatch
#   wp176    Main: a fade, weightp=2 (explicit weighted P, duplicate refs
#            through ref_pic_list_modification), 3 refs, no B
#   wb176    High: the same fade, weightp=2 + weightb=1 (explicit P,
#            implicit bi-prediction), the x264 default combination
#   sl176    Main, CAVLC: 12 slices per picture, I/P/B (more than eight
#            0x81 packets per picture)
#
# Files (pushed to the device, not embedded; same checks with -f):
#   hd720    1280x720 High, 8 frames, IBBP, weightp/weightb, 3 refs
#   fhd1080  1920x1080 High, 6 frames (1088 coded, cropped), IBBP
#   fhd1080s4  the same with 4 slices per picture
#
# --clips additionally writes playback clips for GStreamer on the device
# (x264 preset medium defaults: b-pyramid, weightp/weightb, 8x8, CABAC):
#   clip720.mp4 / clip1080.mp4 (10 s, 30 fps) and their .h264 elementary
#   streams, plus ffmpeg's NV12 decode MD5 (whole stream, .nv12.md5) and
#   one MD5 per frame (.framemd5) for the bit-exact check of the
#   GStreamer output (checksumsink).
#
# Usage: scripts/vdec/make-h264-streams.sh [--clips]
#        (writes build/vdec-r6/streams/ and scripts/vdec/h264-streams.h;
#        re-running is idempotent for a given ffmpeg/libx264 build, the
#        header records the version)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/vdec-r6/streams"
HDR="$ROOT/scripts/vdec/h264-streams.h"
CLIPS=0
[ "${1:-}" = --clips ] && CLIPS=1
mkdir -p "$OUT"

FF=(ffmpeg -hide_banner -loglevel error -y -threads 1)
NOSEI=(-bsf:v filter_units=remove_types=6 -f h264)
BASE=(-c:v libx264 -profile:v baseline -preset medium "${NOSEI[@]}")
X264=(-c:v libx264 -preset medium -threads 1)
DET=keyint=60:scenecut=0:aud=0:threads=1:sliced-threads=0:rc-lookahead=10
BFR=bframes=2:b-adapt=0:b-pyramid=none
TS=(-f lavfi -i testsrc2=s=176x144:r=25)
FADE=(-f lavfi -i "testsrc2=s=176x144:r=25,fade=in:st=0:d=0.32")

# An asymmetric quantiser matrix (JM format, rows of the raster): strong
# along x, mild along y, so a transposed upload decodes visibly wrong.
CQM="$OUT/asym.cqm"
python3 -I - "$CQM" <<'EOF'
import sys
def m(n, base, dx, dy):
    return '\n'.join(','.join(str(min(255, base + dx * c + dy * r)) for c in range(n))
                     for r in range(n))
lists = [('INTRA4X4_LUMA', 4, 8, 12, 2), ('INTRA4X4_CHROMAU', 4, 10, 10, 3),
         ('INTRA4X4_CHROMAV', 4, 12, 9, 4), ('INTER4X4_LUMA', 4, 9, 11, 1),
         ('INTER4X4_CHROMAU', 4, 11, 8, 2), ('INTER4X4_CHROMAV', 4, 13, 7, 3),
         ('INTRA8X8_LUMA', 8, 8, 6, 1), ('INTER8X8_LUMA', 8, 9, 5, 2)]
with open(sys.argv[1], 'w') as f:
    for name, n, base, dx, dy in lists:
        f.write(f'{name} =\n{m(n, base, dx, dy)}\n\n')
EOF

# --- round 5 streams (unchanged recipes) ---
"${FF[@]}" -f lavfi -i color=c=gray:s=64x64:r=25 -frames:v 1 "${BASE[@]}" \
	-x264-params keyint=1:cabac=0:slices=1:aud=0 "$OUT/grey64.h264"
"${FF[@]}" "${TS[@]}" -frames:v 3 -qp 30 "${BASE[@]}" \
	-x264-params keyint=30:cabac=0:slices=1:bframes=0:ref=1:aud=0 "$OUT/ts176.h264"
"${FF[@]}" "${TS[@]}" -frames:v 3 -qp 30 "${BASE[@]}" \
	-x264-params keyint=30:cabac=0:slices=4:bframes=0:ref=1:aud=0 "$OUT/ts176s4.h264"
"${FF[@]}" "${TS[@]}" -frames:v 3 -qp 30 \
	-c:v libx264 -profile:v main -preset medium "${NOSEI[@]}" \
	-x264-params keyint=30:cabac=1:slices=1:bframes=0:ref=1:weightp=0:aud=0 "$OUT/ts176cb.h264"

# --- round 6: High profile, B slices, weights, scaling, many slices ---
"${FF[@]}" "${TS[@]}" -frames:v 7 -qp 28 "${X264[@]}" -profile:v high "${NOSEI[@]}" \
	-x264-params $DET:$BFR:cabac=1:8x8dct=1:direct=spatial:ref=2:weightp=0:weightb=0 "$OUT/hp176.h264"
"${FF[@]}" "${TS[@]}" -frames:v 7 -qp 28 "${X264[@]}" -profile:v high "${NOSEI[@]}" \
	-x264-params $DET:$BFR:cabac=0:8x8dct=1:direct=temporal:ref=2:weightp=0:weightb=0 "$OUT/hp176t.h264"
"${FF[@]}" "${TS[@]}" -frames:v 7 -qp 28 "${X264[@]}" -profile:v high "${NOSEI[@]}" \
	-x264-params $DET:$BFR:cabac=1:8x8dct=1:cqm=jvt:direct=spatial:ref=2:weightp=0:weightb=0 "$OUT/hp176q.h264"
"${FF[@]}" "${TS[@]}" -frames:v 7 -qp 28 "${X264[@]}" -profile:v high "${NOSEI[@]}" \
	-x264-params "$DET:$BFR:cabac=1:8x8dct=1:cqmfile=$CQM:direct=spatial:ref=2:weightp=0:weightb=0" "$OUT/hp176m.h264"
"${FF[@]}" "${FADE[@]}" -frames:v 7 -qp 28 "${X264[@]}" -profile:v main "${NOSEI[@]}" \
	-x264-params $DET:bframes=0:cabac=1:ref=3:weightp=2:weightb=0 "$OUT/wp176.h264"
"${FF[@]}" "${FADE[@]}" -frames:v 7 -qp 28 "${X264[@]}" -profile:v high "${NOSEI[@]}" \
	-x264-params $DET:$BFR:cabac=1:8x8dct=1:ref=2:weightp=2:weightb=1:direct=spatial "$OUT/wb176.h264"
"${FF[@]}" "${TS[@]}" -frames:v 4 -qp 28 "${X264[@]}" -profile:v main "${NOSEI[@]}" \
	-x264-params $DET:bframes=1:b-adapt=0:b-pyramid=none:cabac=0:slices=12:ref=1:weightp=0:weightb=0 "$OUT/sl176.h264"

# --- round 6: large streams (files) ---
"${FF[@]}" -f lavfi -i testsrc2=s=1280x720:r=30 -frames:v 8 -crf 26 "${X264[@]}" -profile:v high \
	"${NOSEI[@]}" -x264-params $DET:$BFR:ref=3:weightp=2:weightb=1 "$OUT/hd720.h264"
"${FF[@]}" -f lavfi -i testsrc2=s=1920x1080:r=30 -frames:v 6 -crf 26 "${X264[@]}" -profile:v high \
	"${NOSEI[@]}" -x264-params $DET:$BFR:ref=3:weightp=2:weightb=1 "$OUT/fhd1080.h264"
"${FF[@]}" -f lavfi -i testsrc2=s=1920x1080:r=30 -frames:v 6 -crf 26 "${X264[@]}" -profile:v high \
	"${NOSEI[@]}" -x264-params $DET:$BFR:ref=3:weightp=2:weightb=1:slices=4 "$OUT/fhd1080s4.h264"

EMBED="grey64 ts176 ts176s4 ts176cb hp176 hp176t hp176q hp176m wp176 wb176 sl176"
FILES="hd720 fhd1080 fhd1080s4"
for s in $EMBED $FILES; do
	"${FF[@]}" -i "$OUT/$s.h264" -pix_fmt nv12 -f rawvideo "$OUT/$s.nv12"
done

python3 -I - "$OUT" "$HDR" "$(ffmpeg -hide_banner -version | head -1)" "$EMBED" "$FILES" <<'EOF'
import sys, zlib, pathlib, subprocess
out, hdr, ver = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
embed, files = sys.argv[4].split(), sys.argv[5].split()

def dims(name):
    r = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                        'stream=width,height', '-of', 'csv=p=0', str(out / f'{name}.h264')],
                       capture_output=True, text=True, check=True)
    w, h = r.stdout.strip().split(',')[:2]
    return int(w), int(h)

def crcs(name, w, h):
    ref = (out / f'{name}.nv12').read_bytes()
    fsz = w * h * 3 // 2
    assert len(ref) % fsz == 0, name
    return [zlib.crc32(ref[i:i + fsz]) for i in range(0, len(ref), fsz)]

L = ['/* SPDX-License-Identifier: GPL-2.0 */',
     '/*',
     ' * Generated by scripts/vdec/make-h264-streams.sh -- do not edit.',
     f' * {ver}',
     ' * Annex-B H.264 test streams (see the script for each one) and the',
     " * CRC-32 (zlib) of each frame of ffmpeg's NV12 decode, in display",
     ' * order (luma rows, then interleaved CbCr, cropped size).',
     ' */',
     '#ifndef VDEC_H264_STREAMS_H',
     '#define VDEC_H264_STREAMS_H',
     '',
     'struct vdec_h264_stream {',
     '\tconst char *name;',
     '\tunsigned int width, height, frames;',
     '\tconst unsigned char *data;',
     '\tunsigned int size;',
     '\tconst unsigned int *ref_crc;\t/* per frame, display order */',
     '};',
     '']
for name in embed:
    w, h = dims(name)
    data = (out / f'{name}.h264').read_bytes()
    c = crcs(name, w, h)
    L.append(f'static const unsigned char vdec_{name}_data[{len(data)}] = {{')
    for i in range(0, len(data), 12):
        L.append('\t' + ' '.join(f'0x{b:02x},' for b in data[i:i + 12]))
    L.append('};')
    L.append(f'static const unsigned int vdec_{name}_crc[{len(c)}] = {{ ' +
             ', '.join(f'0x{x:08x}' for x in c) + ' };')
    L.append('')
L.append('static const struct vdec_h264_stream vdec_h264_streams[] = {')
for name in embed:
    w, h = dims(name)
    n = len(crcs(name, w, h))
    L.append(f'\t{{ "{name}", {w}, {h}, {n}, vdec_{name}_data, '
             f'sizeof(vdec_{name}_data), vdec_{name}_crc }},')
L.append('};')
L.append('')
L.append('#endif')
hdr.write_text('\n'.join(L) + '\n')
for name in embed + files:
    w, h = dims(name)
    c = crcs(name, w, h)
    if name in files:
        (out / f'{name}.crc').write_text(''.join(f'{x:08x}\n' for x in c))
    print(f'{name}: {w}x{h}, {(out / (name + ".h264")).stat().st_size} bytes, {len(c)} frames'
          + ('' if name in embed else f', CRCs in {name}.crc'))
EOF
echo "wrote $HDR"

if [ "$CLIPS" = 1 ]; then
	for c in 720:1280x720 1080:1920x1080; do
		n=${c%%:*}
		s=${c#*:}
		"${FF[@]}" -f lavfi -i testsrc2=s=$s:r=30 -t 10 -c:v libx264 -preset medium \
			-profile:v high -crf 23 -pix_fmt yuv420p -movflags +faststart "$OUT/clip$n.mp4"
		"${FF[@]}" -i "$OUT/clip$n.mp4" -c:v copy -bsf:v h264_mp4toannexb -f h264 "$OUT/clip$n.h264"
		"${FF[@]}" -i "$OUT/clip$n.h264" -pix_fmt nv12 -f rawvideo - | md5 -q > "$OUT/clip$n.nv12.md5"
		# One MD5 per frame (display order, tight NV12): what GStreamer's
		# checksumsink hash=md5 prints on the device.
		"${FF[@]}" -i "$OUT/clip$n.h264" -pix_fmt nv12 -f framemd5 - |
			awk -F', *' '!/^#/ { print $NF }' > "$OUT/clip$n.framemd5"
		echo "clip$n: $(stat -f %z "$OUT/clip$n.mp4") bytes mp4, nv12 md5 $(cat "$OUT/clip$n.nv12.md5"), $(wc -l < "$OUT/clip$n.framemd5" | tr -d ' ') frame MD5s"
	done
fi
