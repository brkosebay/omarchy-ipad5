#!/usr/bin/env bash
# Round 7: the iPad's media stack for the apple-vxd QEMU smoke (boot
# "media"): the ffmpeg package built by scripts/vdec/build-ffmpeg-pkg.sh
# (V4L2 Request API hwaccels) and Arch Linux ARM's stock mpv 1:0.41.0-6,
# reduced to ffmpeg, ffprobe, mpv and the closure of their libraries,
# including Arch's dynamic loader.  The initramfs runs them as
#   /media/lib/ld-linux-aarch64.so.1 --library-path /media/lib /media/bin/mpv
# so they do not mix with the Fedora libraries of the rest of the smoke
# (as scripts/vdec/gst-bundle.sh does for GStreamer).
#
# Both are installed with pacman -r into the build's sysroot on the Mac
# (build/vdec-r7/ffmpeg-work/sysroot, from the image's own package cache:
# the versions the iPad runs) and removed from it again afterwards; the
# container itself installs nothing (Docker's VM disk is nearly full).
#
# Usage: scripts/vdec/media-bundle.sh  -> build/vdec-r7/media/{bin,lib,VERSIONS}
# Needs: build/vdec-r7/pkgs/ffmpeg-2:9.0.2-2.1-aarch64.pkg.tar.xz and the
#        sysroot of scripts/vdec/build-ffmpeg-pkg.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/vdec-r7/media"
SYSROOT="$ROOT/build/vdec-r7/ffmpeg-work/sysroot"
PKG=$(ls "$ROOT"/build/vdec-r7/pkgs/ffmpeg-2:9.0.2-2.1-aarch64.pkg.tar.* 2>/dev/null | head -1)
T="$ROOT/build/omarchy/tmp/pacman-templates"
[ -n "$PKG" ] || { echo "no ffmpeg package: run scripts/vdec/build-ffmpeg-pkg.sh" >&2; exit 1; }
[ -d "$SYSROOT/var/lib/pacman/local" ] || { echo "no sysroot: run scripts/vdec/build-ffmpeg-pkg.sh" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT"

docker run --rm --platform linux/arm64 \
	-v "$T:/t:ro" \
	-v linux-a9-omarchy-pkgcache:/imgcache:ro \
	-v "$(dirname "$PKG"):/pkgs:ro" \
	-v "$SYSROOT:/sysroot" \
	-v "$ROOT/build/vdec-r7/ffmpeg-work/pkg:/var/cache/pacman/pkg" \
	-v "$OUT:/out" \
	-e PKG="$(basename "$PKG")" -e HOST_ID="$(id -u):$(id -g)" \
	linux-a9-alarm:latest bash -euo pipefail -c '
cp /t/pacman-edge.conf /etc/pacman.conf
cp /t/mirrorlist-edge /etc/pacman.d/mirrorlist
sed -i "/^\[omarchy\]/,/^$/d" /etc/pacman.conf
sed -i "/^\[options\]/a DisableSandboxFilesystem" /etc/pacman.conf
pacman-key --init >/dev/null 2>&1
pacman-key --populate archlinuxarm >/dev/null 2>&1
mkdir -p /tmp/nohooks
SR="pacman --noconfirm --root /sysroot --dbpath /sysroot/var/lib/pacman --noscriptlet --hookdir /tmp/nohooks --cachedir /var/cache/pacman/pkg --cachedir /imgcache"
$SR -Sy >/dev/null
# Our ffmpeg first (it satisfies mpv'"'"'s libav* dependencies), then stock mpv.
$SR -U "/pkgs/$PKG" >/tmp/pacman.log 2>&1 || { tail -20 /tmp/pacman.log; exit 1; }
$SR -S --needed mpv >>/tmp/pacman.log 2>&1 || { tail -20 /tmp/pacman.log; exit 1; }
LD=/sysroot/usr/lib/ld-linux-aarch64.so.1
mkdir -p /out/bin /out/lib
cp /sysroot/usr/bin/ffmpeg /sysroot/usr/bin/ffprobe /sysroot/usr/bin/mpv /out/bin/
# Some libraries are linked by absolute path (mpv: NEEDED /usr/lib/libmujs.so);
# the loader cannot redirect those, so they go to the same path here and in
# the initramfs (ABSOLUTE lists them for scripts/vdec/qemu-smoke.sh).
: > /out/ABSOLUTE
for f in /out/bin/*; do
	readelf -d $f | sed -n "s/.*NEEDED.*\[\(\/[^]]*\)\]/\1/p"
done | sort -u | while read -r a; do
	cp -L /sysroot$a $a
	echo $a >> /out/ABSOLUTE
done
# dlopen()ed at run time, so not in any NEEDED list: SDL2 is sdl2-compat,
# which loads SDL3 on first use and aborts mpv without it.
DLOPEN="/sysroot/usr/lib/libSDL3.so.0"
for f in /out/bin/* $DLOPEN; do
	$LD --list --library-path /sysroot/usr/lib:/sysroot/usr/lib/pulseaudio $f
	case $f in /sysroot/*) echo "	$(basename $f) => $f (dlopen)" ;; esac
done > /tmp/closure.txt
if grep -q "not found" /tmp/closure.txt; then echo "media-bundle: unresolved:"; grep "not found" /tmp/closure.txt | sort -u; exit 1; fi
grep -o "/sysroot/[^ ]*" /tmp/closure.txt | sort -u | while read -r l; do cp -L "$l" /out/lib/; done
cp -L $LD /out/lib/ld-linux-aarch64.so.1
while read -r a; do mkdir -p /out/abs$(dirname $a) && cp -L /sysroot$a /out/abs$a; done < /out/ABSOLUTE
# Other libraries mpv and libavutil dlopen() at run time are not in the
# closure; none is needed for --vo=null/--ao=null decoding.
pacman --root /sysroot --dbpath /sysroot/var/lib/pacman -Q ffmpeg mpv glibc libplacebo libdrm systemd-libs > /out/VERSIONS
echo "hwaccels: $(/out/lib/ld-linux-aarch64.so.1 --library-path /out/lib /out/bin/ffmpeg -hide_banner -hwaccels | tail -n +2 | tr "\n" " ")" >> /out/VERSIONS
# Leave the build sysroot as it was.
$SR -Rdd ffmpeg mpv >/dev/null
chown -R "$HOST_ID" /out
'
echo "media-bundle: $(du -sh "$OUT" | cut -f1) in $OUT"
cat "$OUT/VERSIONS"
