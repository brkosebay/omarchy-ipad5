#!/usr/bin/env bash
# Round 7: everything the iPad needs for the media stack, in one directory
# the orchestrator copies over SSH (the iPad has no internet):
#
#   build/vdec-r7/stage/
#     install-media.sh r7-tests.sh mpv.conf 60-apple-vxd-gstreamer.conf vxd-video.html
#     pkgs/      ffmpeg-2:9.0.2-2.1 (scripts/vdec/build-ffmpeg-pkg.sh)
#     rollback/  the stock ffmpeg-2:9.0.2-2 from the image's pacman cache
#     gst/       gstreamer + gst-plugins-{base,good,bad} at the image's
#                versions (scripts/omarchy/fetch-live-pkgs.sh), unless
#                NO_GST=1
#     SHA256SUMS
#
# Usage: scripts/vdec/stage-r7-media.sh
# Then (orchestrator):
#   tar cf - -C build/vdec-r7 stage | scripts/omarchy/ssh.sh 'rm -rf /var/tmp/r7-media && mkdir -p /var/tmp/r7-media && tar xf - -C /var/tmp/r7-media --strip-components=1'
#   scripts/omarchy/ssh.sh 'sudo /var/tmp/r7-media/install-media.sh'
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
S="$ROOT/build/vdec-r7/stage"
PKG=$(ls "$ROOT"/build/vdec-r7/pkgs/ffmpeg-2:9.0.2-2.1-aarch64.pkg.tar.* 2>/dev/null | head -1)
[ -n "$PKG" ] || { echo "no ffmpeg package: run scripts/vdec/build-ffmpeg-pkg.sh" >&2; exit 1; }
grep -q '^ABI: PASS' "$ROOT/build/vdec-r7/pkgs/abi-check.txt" ||
	{ echo "build/vdec-r7/pkgs/abi-check.txt does not say PASS" >&2; exit 1; }
rm -rf "$S"
mkdir -p "$S/pkgs" "$S/rollback"
cp "$ROOT"/scripts/vdec/r7-media/{install-media.sh,r7-tests.sh,mpv.conf,60-apple-vxd-gstreamer.conf,vxd-video.html} "$S/"
chmod +x "$S/install-media.sh" "$S/r7-tests.sh"
cp "$PKG" "$S/pkgs/"
(cd "$S" && shasum -a 256 pkgs/* > SHA256SUMS)

# The stock package, straight from the image's own cache (read-only).
docker run --rm -v linux-a9-omarchy-pkgcache:/c:ro -v "$S/rollback:/out" alpine sh -c \
	'cp /c/ffmpeg-2:9.0.2-2-aarch64.pkg.tar.xz /out/ && cd /out && sha256sum ffmpeg-* > SHA256SUMS && chown -R '"$(id -u):$(id -g)"' /out'

# Not fatal: fetch-live-pkgs.sh stops on a partial upgrade (the mirror ahead
# of the image); install-media.sh then skips GStreamer and round 6's
# option B bundle (scripts/vdec/gst-bundle.sh) still works for the tests.
if [ "${NO_GST:-0}" != 1 ]; then
	"$ROOT/scripts/omarchy/fetch-live-pkgs.sh" --image "$ROOT/build/omarchy/rootfs.img" \
		--out "$S/gst" gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad ||
		{ echo "stage-r7-media: GStreamer packages not staged (see above); use round 6's option B bundle"; rm -rf "$S/gst"; }
fi
echo "stage-r7-media: $(du -sh "$S" | cut -f1) in $S"
ls -R "$S" | head -40
