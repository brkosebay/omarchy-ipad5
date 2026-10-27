#!/usr/bin/env bash
# Base initramfs + the audio test overlay (a9audio, audio-test.sh, devmem,
# test WAVs, SmartIO firmware) -> build/audio/initramfs-audio.gz as ONE gzip stream (m1n1 keeps
# only the last gzip member, so concatenated members must be repacked).
#
#   scripts/audio/mkinitramfs.sh [BASE_INITRAMFS.gz]
#   default base: build/initramfs-fw.gz if present, else the HoolockRD prebuilt
# Then: TREE=audio scripts/mkpayload.sh build/audio/initramfs-audio.gz
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="${1:-}"
if [ -z "$BASE" ]; then
	BASE="$ROOT/build/initramfs-fw.gz"
	[ -f "$BASE" ] || BASE="$ROOT/build/prebuilt/initramfs/initramfs.gz"
fi
[ -f "$ROOT/build/audio/audio-tools.cpio.gz" ] || "$ROOT/scripts/audio/build-tools.sh"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/usr/local/bin" "$STAGE/lib/firmware/apple"
cp "$ROOT/build/device-bin/devmem" "$STAGE/usr/local/bin/devmem"
# SmartIO firmware for the SIO DMA (already in initramfs-fw.gz; any base works)
cp "$ROOT/build/firmware/lib/firmware/apple/s8000-smartio.bin" "$STAGE/lib/firmware/apple/"
(cd "$STAGE" && find . | LC_ALL=C sort | cpio -o -H newc --quiet) > "$STAGE.devmem.cpio"

OUT="$ROOT/build/audio/initramfs-audio.gz"
{ gzip -dc "$BASE"; gzip -dc "$ROOT/build/audio/audio-tools.cpio.gz"; cat "$STAGE.devmem.cpio"; } \
	| gzip -9n > "$OUT"
rm -f "$STAGE.devmem.cpio"
echo "base: $BASE"
echo "out:  $OUT ($(wc -c < "$OUT") bytes, single gzip stream)"
