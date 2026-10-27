#!/usr/bin/env bash
# Round 7: build scripts/vdec/ffmpeg-v4l2request/PKGBUILD (Arch Linux ARM's
# ffmpeg 2:9.0.2-2 + the V4L2 Request API hwaccels) into an installable
# aarch64 package, in a throwaway linux-a9-alarm container.
#
# Docker's VM disk is shared by every lane and nearly full, so the container
# only gets the toolchain (gcc, make, pkgconf, perl, fakeroot, glslang).
# Every library the PKGBUILD depends on is installed with pacman -r into a
# sysroot on the Mac (build/vdec-r7/ffmpeg-work/sysroot), from the Omarchy
# image's own pacman cache (linux-a9-omarchy-pkgcache, read-only: the exact
# versions the iPad runs) and, for what is not there, the image's mirror.
# gcc and pkg-config wrappers point the build at that sysroot; makepkg runs
# with --nodeps.  The tarball's detached signature is checked against the
# FFmpeg release key (FCF986EA15E6E293A5644F10B4322F04D67658D8) before
# makepkg's sha256 check.
#
# Then a compatibility check against the stock package from the same cache:
# every library keeps its soname, NEEDED gains at most libudev, no exported
# symbol disappears, and the decoder/encoder/filter/muxer/demuxer/protocol/
# bsf lists are identical (the hwaccel list gains the v4l2request ones).
# The package is only a drop-in replacement if all of that holds.
#
# Usage: scripts/vdec/build-ffmpeg-pkg.sh   (JOBS=2 by default: the Docker
#        VM has 5.8 GiB for every lane, -j4 got cc1 OOM-killed)
# Output: build/vdec-r7/pkgs/{ffmpeg-2:9.0.2-2.1-aarch64.pkg.tar.xz,
#         SHA256SUMS, abi-check.txt, build.log}
# Network: ffmpeg.org (tarball cached in build/vdec-r7/ffmpeg-work/dl),
# keyserver.ubuntu.com or keys.openpgp.org, the image's ALARM mirror.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/vdec-r7/pkgs"
T="$ROOT/build/omarchy/tmp/pacman-templates"
W="$ROOT/build/vdec-r7/ffmpeg-work"
JOBS="${JOBS:-2}"
[ -f "$T/pacman-edge.conf" ] || { echo "missing $T (scripts/omarchy/build-rootfs.sh makes it)" >&2; exit 1; }
# A fresh build directory per run: deleting and recreating the same path
# right before docker run leaves Docker Desktop's file sharing with a stale
# handle (the bind mount pointed at the deleted directory).  RESUME=1 reuses
# the newest one and runs makepkg --noextract (after a failure late in the
# build; configure leaves unchanged objects alone).
B=""
[ "${RESUME:-0}" = 1 ] && B=$(ls -d "$W"/build-* 2>/dev/null | tail -1)
[ -n "$B" ] || B="$W/build-$(date +%Y%m%d-%H%M%S)"
for old in "$W"/build "$W"/build-*; do
	[ -e "$old" ] && [ "$old" != "$B" ] && rm -rf "$old"
done
mkdir -p "$OUT" "$B" "$W/pkg" "$W/sysroot" "$W/dl"
chmod 777 "$B"
for f in ffmpeg-9.0.2.tar.xz ffmpeg-9.0.2.tar.xz.asc; do
	[ -s "$W/dl/$f" ] || curl -sSfL --retry 5 --retry-all-errors -o "$W/dl/$f" "https://ffmpeg.org/releases/$f"
done
rm -f "$OUT"/ffmpeg-*.pkg.tar.* "$OUT/abi-check.txt"

docker run --rm --platform linux/arm64 \
	-v "$ROOT/scripts/vdec/ffmpeg-v4l2request:/pkgsrc:ro" \
	-v "$T:/t:ro" \
	-v linux-a9-omarchy-pkgcache:/imgcache:ro \
	-v "$OUT:/out" \
	-v "$B:/build" \
	-v "$W/pkg:/var/cache/pacman/pkg" \
	-v "$W/sysroot:/sysroot" \
	-v "$W/dl:/dl:ro" \
	-e HOST_ID="$(id -u):$(id -g)" -e JOBS="$JOBS" -e RESUME="${RESUME:-0}" \
	linux-a9-alarm:latest bash -euo pipefail -c '
cp /t/pacman-edge.conf /etc/pacman.conf
cp /t/mirrorlist-edge /etc/pacman.d/mirrorlist
# Only the ALARM repos: nothing here comes from [omarchy].
sed -i "/^\[omarchy\]/,/^$/d" /etc/pacman.conf
sed -i "/^\[options\]/a DisableSandboxFilesystem" /etc/pacman.conf
pacman-key --init >/dev/null 2>&1
pacman-key --populate archlinuxarm >/dev/null 2>&1
PM="pacman --noconfirm --cachedir /var/cache/pacman/pkg --cachedir /imgcache"
$PM -Sy >/dev/null
# glslang: FFmpeg 9 compiles its Vulkan shaders at build time (the ALARM
# builder gets it through libplacebo); without it configure silently drops
# every *_vulkan filter, encoder and hwaccel.
$PM -S --needed gcc make pkgconf patch fakeroot perl diffutils glslang >/tmp/pacman.log 2>&1 ||
	{ tail -20 /tmp/pacman.log; exit 1; }
echo "toolchain: $(pacman -Q gcc binutils glibc glslang | tr "\n" " ")"

# The sysroot: glibc, the linux headers and the PKGBUILD'"'"'s dependencies,
# without install scriptlets (nothing runs in there; only headers and
# libraries are used).
deps=$(bash -c "source /pkgsrc/PKGBUILD; echo \${depends[*]} \${makedepends[*]}")
mkdir -p /sysroot/var/lib/pacman /tmp/nohooks
SR="$PM --root /sysroot --dbpath /sysroot/var/lib/pacman --noscriptlet --hookdir /tmp/nohooks"
$SR -Sy >/dev/null
$SR -S --needed glibc linux-api-headers $deps >/tmp/sysroot.log 2>&1 ||
	{ grep -vE "is up to date" /tmp/sysroot.log | tail -20; exit 1; }
echo "sysroot: $(pacman --root /sysroot --dbpath /sysroot/var/lib/pacman -Q | wc -l) packages"
for p in x265 libplacebo dav1d libvpx x264 libjxl srt libssh systemd-libs libdrm; do
	have=$(pacman --root /sysroot --dbpath /sysroot/var/lib/pacman -Q $p | cut -d" " -f2)
	ls /imgcache/$p-$have-*.pkg.tar.* >/dev/null 2>&1 && img=same || img="NOT the image version"
	echo "  $p $have ($img)"
done

# Point the compiler and pkg-config at the sysroot.
mkdir -p /opt/sr/bin
for t in gcc cc g++ c++; do
	printf "#!/bin/sh\nexec /usr/bin/%s --sysroot=/sysroot -Wl,-rpath-link=/sysroot/usr/lib \"\$@\"\n" $t > /opt/sr/bin/$t
done
printf "#!/bin/sh\nPKG_CONFIG_SYSROOT_DIR=/sysroot PKG_CONFIG_LIBDIR=/sysroot/usr/lib/pkgconfig:/sysroot/usr/share/pkgconfig exec /usr/bin/pkgconf \"\$@\"\n" > /opt/sr/bin/pkg-config
chmod +x /opt/sr/bin/*

useradd -m builder
cp /pkgsrc/* /build/ && chown -R builder /build
MPFLAGS="--nodeps --noconfirm --nocheck"
[ "$RESUME" = 1 ] && [ -d /build/src/ffmpeg-9.0.2 ] && MPFLAGS="$MPFLAGS --noextract"
cd /build
cp /dl/ffmpeg-9.0.2.tar.xz /dl/ffmpeg-9.0.2.tar.xz.asc /build/ && chown builder /build/ffmpeg-9.0.2.tar.xz*
export GNUPGHOME=$(mktemp -d)
for ks in hkps://keyserver.ubuntu.com hkps://keys.openpgp.org hkps://keyserver.ubuntu.com; do
	gpg -q --keyserver $ks --recv-keys FCF986EA15E6E293A5644F10B4322F04D67658D8 2>/dev/null && break
	sleep 5
done
gpg --status-fd 1 --verify ffmpeg-9.0.2.tar.xz.asc ffmpeg-9.0.2.tar.xz 2>/dev/null |
	grep -q "VALIDSIG FCF986EA15E6E293A5644F10B4322F04D67658D8" ||
	{ echo "ffmpeg-9.0.2.tar.xz: bad or missing signature" >&2; exit 1; }
echo "tarball signature: good (FFmpeg release signing key FCF986EA...58D8)"

mkdir -p /build/tmp && chown builder /build/tmp
if [ "$RESUME" = 1 ] && ls /build/out/ffmpeg-*.pkg.tar.* >/dev/null 2>&1; then
	echo "RESUME: reusing $(ls /build/out/ffmpeg-*.pkg.tar.*)"
else
	su builder -c "PATH=/opt/sr/bin:/usr/bin:/usr/bin/core_perl TMPDIR=/build/tmp MAKEFLAGS=-j$JOBS PKGDEST=/build/out makepkg $MPFLAGS" > /build/build.log 2>&1 ||
		{ tail -40 /build/build.log; cp /build/build.log /out/; exit 1; }
fi
cp /build/build.log /out/
grep -E "^(CONFIG_V4L2_REQUEST|CONFIG_H264_V4L2REQUEST_HWACCEL|CONFIG_HEVC_V4L2REQUEST_HWACCEL|CONFIG_LIBUDEV)=" /build/src/ffmpeg-9.0.2/ffbuild/config.mak
new=$(ls /build/out/ffmpeg-*.pkg.tar.*)
old=$(ls /imgcache/ffmpeg-2:9.0.2-2-aarch64.pkg.tar.*z | grep -v sig)

# Compatibility check against the stock package.  Both ffmpeg binaries run
# on the sysroot'"'"'s loader and libraries.
mkdir -p /abi/new /abi/old
tar -xf "$new" -C /abi/new usr/lib usr/bin
tar -xf "$old" -C /abi/old usr/lib usr/bin
run() { d=$1; shift; /sysroot/usr/lib/ld-linux-aarch64.so.1 --library-path /abi/$d/usr/lib:/sysroot/usr/lib:/sysroot/usr/lib/pulseaudio /abi/$d/usr/bin/ffmpeg -hide_banner "$@"; }
{
echo "ABI check: $(basename "$new") against $(basename "$old")"
rc=0
for lib in /abi/old/usr/lib/lib*.so.*.*.*; do
	b=$(basename "$lib"); n=/abi/new/usr/lib/$b
	[ -f "$n" ] || { echo "MISSING $b"; rc=1; continue; }
	so_o=$(readelf -d "$lib" | sed -n "s/.*SONAME.*\[\(.*\)\]/\1/p")
	so_n=$(readelf -d "$n" | sed -n "s/.*SONAME.*\[\(.*\)\]/\1/p")
	[ "$so_o" = "$so_n" ] || { echo "SONAME $b: $so_o -> $so_n"; rc=1; }
	need_add=$(comm -13 <(readelf -d "$lib" | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p" | sort) \
		<(readelf -d "$n" | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p" | sort) | tr "\n" " ")
	need_del=$(comm -23 <(readelf -d "$lib" | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p" | sort) \
		<(readelf -d "$n" | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p" | sort) | tr "\n" " ")
	sym_del=$(comm -23 <(nm -D --defined-only "$lib" | awk "{print \$3}" | sort) \
		<(nm -D --defined-only "$n" | awk "{print \$3}" | sort) | tr "\n" " ")
	sym_add=$(comm -13 <(nm -D --defined-only "$lib" | awk "{print \$3}" | sort) \
		<(nm -D --defined-only "$n" | awk "{print \$3}" | sort) | tr "\n" " ")
	echo "$b soname $so_n; NEEDED +[${need_add% }] -[${need_del% }]; symbols -[${sym_del% }] +[${sym_add% }]"
	[ -z "$sym_del" ] || rc=1
	[ -z "$need_del" ] || rc=1
	case "${need_add% }" in ""|libudev.so.1) ;; *) rc=1 ;; esac
done
for l in decoders encoders filters muxers demuxers protocols bsfs; do
	diff <(run old -$l 2>&1) <(run new -$l 2>&1) | grep "^[<>]" > /tmp/d.txt || true
	echo "-$l: $([ -s /tmp/d.txt ] && echo "DIFFERENT: $(tr "\n" " " < /tmp/d.txt)" || echo identical)"
	# Only CUDA filters may differ (no --enable-cuda-llvm; ALARM builds
	# none of them anyway, it has no ffnvcodec-headers).
	grep -qv cuda /tmp/d.txt && rc=1
done
echo "-hwaccels added: $(diff <(run old -hwaccels) <(run new -hwaccels) | sed -n "s/^> //p" | tr "\n" " " || true)"
echo "h264 decoder hw configs: $(run new -h decoder=h264 | sed -n "/hardware/,\$p" | tr -s " \n" " " || true)"
echo "ABI: $([ $rc = 0 ] && echo PASS || echo FAIL)"
} | tee /out/abi-check.txt
cp "$new" /out/
cd /out && sha256sum "$(basename "$new")" > SHA256SUMS
chown -R "$HOST_ID" /out /build
grep -q "^ABI: PASS" /out/abi-check.txt
'
[ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$B"
echo "build-ffmpeg-pkg: $(ls "$OUT"/ffmpeg-*.pkg.tar.*) ($(du -h "$OUT"/ffmpeg-*.pkg.tar.* | cut -f1))"
