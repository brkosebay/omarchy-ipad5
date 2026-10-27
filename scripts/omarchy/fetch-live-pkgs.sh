#!/usr/bin/env bash
# Download aarch64 packages for the running iPad, which has no internet yet,
# so they can be copied over and installed offline with pacman -U.
#
#   scripts/omarchy/fetch-live-pkgs.sh [--image FILE] [--out DIR] [PKG...]
#     PKG     default: the PipeWire audio set the image build now installs
#             (phase "applications", minus pipewire-jack, which would swap
#             out jack2 on a live system)
#     --image the image whose pacman database says what is installed
#             [build/omarchy/rootfs.pristine.img: same packages as the live
#             image, and not being written; rootfs.img is read-only safe too]
#     --out   [build/omarchy/live-pkgs]
#
# Runs in the linux-a9-alarm container with Omarchy's edge pacman templates
# (the image's repos), resolving dependencies against the image's own
# /var/lib/pacman/local, so it fetches exactly what the device lacks. It
# stops if a package would need a newer version of something installed (a
# partial upgrade); the repos still match the image (pipewire 1:1.6.9-1).
#
# Then, on the Mac (docs/omarchy/round3-live.md):
#   scp -r build/omarchy/live-pkgs linux@172.16.42.1:/var/tmp/
#   scripts/omarchy/ssh.sh 'cd /var/tmp/live-pkgs && sha256sum -c --quiet SHA256SUMS && sudo pacman -U --needed --noconfirm $(cat list.txt)'
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/omarchy"
IMAGE_FILE="$OUT/rootfs.pristine.img"
DEST="$OUT/live-pkgs"
pkgs=()
while [ $# -gt 0 ]; do
	case "$1" in
	--image) IMAGE_FILE="$2"; shift 2 ;;
	--out) DEST="$2"; shift 2 ;;
	-h | --help) sed -n '2,24p' "$0"; exit 0 ;;
	*) pkgs+=("$1"); shift ;;
	esac
done
[ ${#pkgs[@]} -gt 0 ] || pkgs=(pipewire-audio alsa-card-profiles pipewire-alsa pipewire-pulse gst-plugin-pipewire libpulse)
[ -f "$IMAGE_FILE" ] || { echo "missing $IMAGE_FILE" >&2; exit 1; }
[ -d "$OUT/tmp/pacman-templates" ] || { echo "missing $OUT/tmp/pacman-templates (run scripts/omarchy/build-rootfs.sh once)" >&2; exit 1; }
rm -rf "$DEST" && mkdir -p "$DEST"

docker run --rm --platform linux/arm64 \
	-v "$IMAGE_FILE:/image.img:ro" \
	-v "$OUT/tmp/pacman-templates:/t:ro" \
	-v "$OUT/src/iso-omacom:/iso:ro" \
	-v "$DEST:/dest" \
	-e PKGS="${pkgs[*]}" \
	linux-a9-alarm:latest bash -euo pipefail -c '
cp /t/pacman-edge.conf /etc/pacman.conf
cp /t/mirrorlist-edge /etc/pacman.d/mirrorlist
# Docker Desktop has no Landlock for pacman'"'"'s download sandbox.
sed -i "/^\[options\]/a DisableSandboxFilesystem" /etc/pacman.conf
pacman-key --init >/dev/null 2>&1
pacman-key --populate archlinuxarm >/dev/null 2>&1
pacman-key --add /iso/builder/omarchy.gpg >/dev/null 2>&1
pacman-key --lsign-key 40DFB630FF42BCFFB047046CF0134EE680CAC571 >/dev/null 2>&1

# The image'"'"'s installed-package database, read without mounting it.
db=/tmp/imagedb
mkdir -p "$db/sync"
debugfs -R "rdump /var/lib/pacman/local $db" /image.img >/dev/null 2>&1
[ -d "$db/local" ] || { echo "could not read /var/lib/pacman/local from the image" >&2; exit 1; }
echo "image: $(ls "$db/local" | wc -l) installed packages"
pacman -Sy --dbpath "$db" >/dev/null

# What the device lacks, and nothing that would upgrade what it has.
targets=$(pacman -Sp --print-format "%n %v" --needed --dbpath "$db" $PKGS)
echo "$targets" | sed "s/^/  /"
while read -r name version; do
	have=$(pacman -Q --dbpath "$db" "$name" 2>/dev/null | cut -d" " -f2 || true)
	if [ -n "$have" ] && [ "$have" != "$version" ]; then
		echo "$name is $have on the device but the repo has $version: that is a partial upgrade, stopping" >&2
		exit 1
	fi
done <<<"$targets"
pacman -Sw --noconfirm --needed --dbpath "$db" --cachedir /dest $PKGS >/dev/null
cd /dest && ls *.pkg.tar.* | grep -v "\.sig$" >list.txt && sha256sum $(cat list.txt) >SHA256SUMS
echo "downloaded $(wc -l <list.txt) packages ($(du -sh . | cut -f1))"
'
echo "-> $DEST"
echo "next: scp -r $DEST linux@172.16.42.1:/var/tmp/  (see docs/omarchy/round3-live.md)"
