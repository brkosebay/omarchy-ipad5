#!/usr/bin/env bash
# Host check of the apple-vxd H.264 command-stream builder (no device, no
# QEMU): compile the driver's pure files from the vdec tree against
# scripts/vdec/host/include shims, run them over the embedded test streams
# (scripts/vdec/h264-streams.h), and diff every picture word for word
# against the independent reference generator tools/ds-video/cmdstream.py
# --gen (docs/periph/ds-vxd-cmdstream.md section 16).
#
# Usage: scripts/vdec/host-cmdstream.sh    (TREE=vdec by default; the 720p and
#        1080p files come from scripts/vdec/make-h264-streams.sh)
# Output: build/vdec-r6/host/{vdec-h264-host,host.jsonl,compare.txt}
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-vdec}"
mkdir -p "$ROOT/build/vdec-r6/host"
"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -e
D=drivers/media/platform/apple/vxd
H=/tmp/vxd-uapi
rm -rf $H && make -s ARCH=arm64 headers_install INSTALL_HDR_PATH=$H >/dev/null
O=/work/build/vdec-r6/host
clang -std=gnu11 -O1 -g0 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-Wno-missing-field-initializers -Werror=implicit-function-declaration \
	-I/work/scripts/vdec/host/include -I$H/include -I$D -idirafter include \
	-o $O/vdec-h264-host /work/scripts/vdec/host/vdec-h264-host.c \
	$D/apple-vxd-cmd.c $D/apple-vxd-h264-parse.c $D/apple-vxd-h264-cmd.c \
	drivers/media/v4l2-core/v4l2-h264.c
$O/vdec-h264-host > $O/host.jsonl 2> $O/host.err; cat $O/host.err
F=/work/build/vdec-r6/streams
$O/vdec-h264-host -f $F/hd720.h264 $F/fhd1080.h264 $F/fhd1080s4.h264 >> $O/host.jsonl 2> $O/host.err; cat $O/host.err
echo "host harness: $(wc -l < $O/host.jsonl) pictures from $(git rev-parse --short HEAD)$(git diff --quiet -- $D || echo " + uncommitted driver changes")"
'
python3 -I "$ROOT/scripts/vdec/host/compare.py" "$ROOT/build/vdec-r6/host/host.jsonl" | tee "$ROOT/build/vdec-r6/host/compare.txt"
