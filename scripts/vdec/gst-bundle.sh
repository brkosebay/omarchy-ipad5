#!/usr/bin/env bash
# GStreamer for the apple-vxd QEMU smoke: the Arch Linux ARM packages the
# iPad would install (gstreamer, gst-plugins-{base,good,bad}), taken from
# a throwaway linux-a9-alarm container, reduced to gst-launch-1.0 /
# gst-inspect-1.0, the plugins the device pipelines use and the ldd
# closure of all of them, including Arch's own dynamic loader.  The
# initramfs runs them as
#   /gst/lib/ld-linux-aarch64.so.1 --library-path /gst/lib /gst/bin/gst-launch-1.0
# so they do not mix with the Fedora libraries of the rest of the smoke.
#
# Plugins: coreelements (filesrc, filesink, fakesink, fdsink),
# videoparsersbad (h264parse), v4l2codecs (v4l2slh264dec), debugutilsbad
# (fpsdisplaysink, checksumsink, fakevideosink), isomp4 (qtdemux),
# videoconvertscale (videoconvert), and for gst-play-1.0 on the iPad's
# Hyprland: playback (playbin), typefindfunctions, autodetect, waylandsink.
# The same bundle runs on the device without pacman (vdec-test-plan.md,
# round 6, T12 option B).
#
# Usage: scripts/vdec/gst-bundle.sh   -> build/vdec-r6/gst/{bin,lib,plugins,VERSIONS}
# Network: the Arch Linux ARM mirror (packages verified with the
# archlinuxarm keyring).  The container is removed afterwards.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/vdec-r6/gst"
rm -rf "$OUT"
mkdir -p "$OUT"

docker run --rm --platform linux/arm64 -v "$OUT:/out" linux-a9-alarm:latest bash -c '
set -euo pipefail
pacman-key --init >/dev/null 2>&1
pacman-key --populate archlinuxarm >/dev/null 2>&1
PM="pacman --disable-sandbox --noconfirm"
$PM -Sy >/dev/null 2>&1
# -dd: only these, not the plugin packages'"'"' long dependency lists; the
# libraries the chosen plugins really load are added below.
$PM -Sdd --needed gstreamer gst-plugins-base-libs gst-plugins-bad-libs \
	gst-plugins-bad gst-plugins-good gst-plugins-base libunwind libdrm orc \
	libgudev wayland libxkbcommon >/dev/null
P=/usr/lib/gstreamer-1.0
PLUGINS="coreelements videoparsersbad v4l2codecs debugutilsbad isomp4 videoconvertscale
	playback typefindfunctions autodetect waylandsink"
mkdir -p /out/bin /out/lib /out/plugins
cp /usr/bin/gst-launch-1.0 /usr/bin/gst-inspect-1.0 /usr/bin/gst-play-1.0 /out/bin/
for p in $PLUGINS; do cp $P/libgst$p.so /out/plugins/; done
missing=$(for f in /out/bin/* /out/plugins/*; do ldd $f; done | grep "not found" | sort -u || true)
if [ -n "$missing" ]; then echo "gst-bundle: unresolved libraries:"; echo "$missing"; exit 1; fi
for f in /out/bin/* /out/plugins/*; do
	ldd $f | grep -o "/[^ ]*" || true
done | sort -u | while read -r l; do cp -L "$l" /out/lib/; done
pacman -Q gstreamer gst-plugins-base-libs gst-plugins-bad-libs gst-plugins-bad \
	gst-plugins-good gst-plugins-base glibc glib2 > /out/VERSIONS
chown -R '"$(id -u):$(id -g)"' /out
'
echo "gst-bundle: $(du -sh "$OUT" | cut -f1) in $OUT"
cat "$OUT/VERSIONS"
