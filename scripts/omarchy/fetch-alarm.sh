#!/usr/bin/env bash
# Download the Arch Linux ARM aarch64 root tarball and check it (md5 + the
# Arch Linux ARM build key's signature), then import it as the Docker image
# that builds the Omarchy rootfs (scripts/omarchy/build-rootfs.sh).
#
# Usage: scripts/omarchy/fetch-alarm.sh [--keep]
#   --keep  keep the tarball in build/omarchy/dl after the import
#           (default: delete it, the host disk is tight)
# Output: Docker image linux-a9-alarm:latest (+ :YYYYMMDD of the tarball)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DL="$ROOT/build/omarchy/dl"
URL="http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz"
TAR="$DL/ArchLinuxARM-aarch64-latest.tar.gz"
# Arch Linux ARM Build System <maintainer@example.invalid>
KEY="68B3537F39A313B3E574D06777193F152BDBE6A6"
IMAGE="linux-a9-alarm"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

mkdir -p "$DL"
cd "$DL"
curl -fsSL -o "$TAR.md5" "$URL.md5"
curl -fsSL -o "$TAR.sig" "$URL.sig"
if [ ! -f "$TAR" ] || [ "$(md5 -q "$TAR" 2>/dev/null || md5sum "$TAR" | cut -d' ' -f1)" != "$(cut -d' ' -f1 "$TAR.md5")" ]; then
	curl -fsSLR --retry 3 -o "$TAR" "$URL"
fi

sum="$(md5 -q "$TAR" 2>/dev/null || md5sum "$TAR" | cut -d' ' -f1)"
[ "$sum" = "$(cut -d' ' -f1 "$TAR.md5")" ] || { echo "md5 mismatch: $sum" >&2; exit 1; }
echo "md5 ok: $sum"

# Verify with a throwaway keyring, so the host's own gpg setup is untouched.
GNUPGHOME="$(mktemp -d)"
export GNUPGHOME
trap 'rm -rf "$GNUPGHOME"' EXIT
gpg --batch --quiet --keyserver hkps://keyserver.ubuntu.com --recv-keys "$KEY"
gpg --batch --status-fd 1 --verify "$TAR.sig" "$TAR" 2>/dev/null | grep -q "VALIDSIG $KEY" ||
	{ echo "signature check failed for $TAR" >&2; exit 1; }
echo "signature ok: $KEY"

stamp="$(date -r "$TAR" +%Y%m%d)"
docker import --platform linux/arm64 "$TAR" "$IMAGE:$stamp" >/dev/null
docker tag "$IMAGE:$stamp" "$IMAGE:latest"
echo "imported $IMAGE:$stamp"

if [ "$KEEP" = 0 ]; then
	rm -f "$TAR"
fi
