#!/usr/bin/env bash
# Build the static device-side audio test tools and test WAVs into
# build/audio/device/ (served to the iPad like tools/devmem):
#   a9audio    static aarch64 (musl) ALSA player/mixer, tools/audio/a9audio.c
#   *.wav      test signals from tools/audio/mkwav.py
#   audio-test.sh  the device-side test script (tools/audio/audio-test.sh)
# Also packs them as build/audio/audio-tools.cpio.gz: one gzip stream,
# to append to an initramfs (scripts/mkpayload.sh repacks to one stream).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/audio/device"
mkdir -p "$OUT"

docker run --rm --platform linux/arm64 \
	-v "$ROOT/tools/audio:/src:ro" -v "$OUT:/out" alpine:3.22 sh -euc '
apk add --no-cache gcc musl-dev linux-headers >/dev/null
gcc -static -O2 -Wall -o /out/a9audio /src/a9audio.c -lm
strip /out/a9audio
'
file "$OUT/a9audio"

python3 -I "$ROOT/tools/audio/mkwav.py" "$OUT"
cp "$ROOT/tools/audio/audio-test.sh" "$OUT/audio-test.sh"
chmod +x "$OUT/audio-test.sh"

# initramfs overlay: /usr/local/bin/{a9audio,audio-test.sh}, /usr/share/a9audio/*.wav
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/usr/local/bin" "$STAGE/usr/share/a9audio"
cp "$OUT/a9audio" "$OUT/audio-test.sh" "$STAGE/usr/local/bin/"
cp "$OUT"/*.wav "$STAGE/usr/share/a9audio/"
(cd "$STAGE" && find . | LC_ALL=C sort | cpio -o -H newc --quiet) | gzip -9 > "$ROOT/build/audio/audio-tools.cpio.gz"
ls -la "$OUT" "$ROOT/build/audio/audio-tools.cpio.gz"
