#!/usr/bin/env bash
# Build a static aarch64 nbd-client (NetworkBlockDevice/nbd) for the NBD-root
# initramfs, in an Alpine arm64 container (musl, fully static).
#
# HoolockRD's BusyBox has an nbd-client applet, but it only speaks the ioctl
# interface and can't mark itself for systemd (argv[0] '@'), so a shutdown
# would kill the process holding the root device. Upstream nbd-client does
# netlink (the kernel owns the socket; nothing to kill) and -systemd-mark.
#
# Usage: scripts/omarchy/build-nbd-client.sh [version]   (default 3.26.1)
# Output: build/omarchy/initramfs/nbd-client (+ nbd-client.version)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:-3.26.1}"
OUT="$ROOT/build/omarchy/initramfs"
mkdir -p "$OUT"

docker run --rm --platform linux/arm64 -v "$OUT:/out" -e VERSION="$VERSION" alpine:3.22 sh -euc '
apk add --no-cache build-base linux-headers pkgconf bison flex \
	glib-dev glib-static libnl3-dev libnl3-static gettext-static pcre2-static zlib-static curl xz >/dev/null
cd /tmp
curl -fsSL -o nbd.tar.xz "https://github.com/NetworkBlockDevice/nbd/releases/download/nbd-$VERSION/nbd-$VERSION.tar.xz"
tar -xf nbd.tar.xz
cd "nbd-$VERSION"
# configure.ac errors out whenever docbook2man is missing, even with
# --disable-manpages; presetting DB2M gets past it (only nbd-client is built).
DB2M=true ./configure --without-gnutls CFLAGS="-O2 -Wno-error=incompatible-pointer-types" >/tmp/configure.log 2>&1 ||
	{ tail -30 /tmp/configure.log; exit 1; }
grep -E "^#define HAVE_NETLINK|^#define HAVE_LIBNL" config.h || echo "note: built without netlink"
# libtool drops a plain -static when linking programs; -all-static survives.
make -j"$(nproc)" nbd-client LDFLAGS="-all-static" LIBS="$(pkg-config --static --libs libnl-genl-3.0 glib-2.0 gthread-2.0)" >/tmp/make.log 2>&1 || { tail -30 /tmp/make.log; exit 1; }
strip nbd-client
apk add --no-cache file >/dev/null && file nbd-client
file nbd-client | grep -q "statically linked" || { echo "nbd-client is not static" >&2; exit 1; }
cp nbd-client /out/nbd-client
./nbd-client -V 2>&1 | head -1 >/out/nbd-client.version || true
'
echo "built: $OUT/nbd-client ($(cat "$OUT/nbd-client.version" 2>/dev/null))"
