#!/usr/bin/env bash
# Fetch the pieces of the boot chain we don't build ourselves:
#   - m1n1 (HoolockLinux idevice branch, CI build)
#   - HoolockRD example initramfs (USB serial/ethernet/telnet shell)
#   - pongoOS (prebuilt Pongo.bin from HoolockLinux/docs)
#   - pongoterm (built from checkra1n/pongoOS source with the system clang)
#
# Output: build/prebuilt/{m1n1/m1n1.bin,initramfs/initramfs.gz,pongo/Pongo.bin}
#         build/bin/pongoterm
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRE="$ROOT/build/prebuilt"
BIN="$ROOT/build/bin"
mkdir -p "$PRE"/{m1n1,initramfs,pongo,pongoterm-src} "$BIN"

fetch_zip() { # url dest_dir
	local tmp
	tmp="$(mktemp -d)"
	curl -fsSL -o "$tmp/a.zip" "$1"
	unzip -oq "$tmp/a.zip" -d "$2"
	rm -rf "$tmp"
}

echo "[*] m1n1 (idevice CI build)"
fetch_zip "https://nightly.link/hoolocklinux/m1n1/workflows/build/idevice/m1n1.zip" "$PRE/m1n1"

echo "[*] HoolockRD initramfs"
fetch_zip "https://nightly.link/HoolockLinux/HoolockRD/workflows/build/master/initramfs.gz.zip" "$PRE/initramfs"

echo "[*] Pongo.bin"
curl -fsSL -o "$PRE/pongo/Pongo.bin" \
	"https://github.com/HoolockLinux/docs/raw/master/binaries/Pongo.bin"

echo "[*] pongoterm"
curl -fsSL -o "$PRE/pongoterm-src/pongoterm.c" \
	"https://raw.githubusercontent.com/checkra1n/pongoOS/iOS15/scripts/pongoterm.c"
clang -Os -x objective-c -framework IOKit -framework CoreFoundation \
	-framework Foundation -lobjc -mmacosx-version-min=10.8 \
	"$PRE/pongoterm-src/pongoterm.c" -o "$BIN/pongoterm"

find "$PRE" "$BIN" -type f -not -name '*.c' -exec ls -la {} +
