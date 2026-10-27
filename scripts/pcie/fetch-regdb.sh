#!/usr/bin/env bash
# Fetch the wireless regulatory database (regulatory.db + its signature
# regulatory.db.p7s) for cfg80211, from Fedora's wireless-regdb package,
# inside the kernel build container.
#
#   scripts/pcie/fetch-regdb.sh      -> build/firmware/regdb/regulatory.db{,.p7s}
#
# cfg80211 is built in, so it reads /lib/firmware/regulatory.db from the
# initramfs about 1 s into boot; scripts/pcie/mkinitramfs-wifi.sh puts these
# files there. The signature is checked against the kernel's built-in
# wireless-regdb keys (CONFIG_CFG80211_USE_KERNEL_REGDB_KEYS).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/firmware/regdb"
IMAGE="linux-a9-kbuild"
mkdir -p "$OUT"

docker run --rm --platform linux/arm64 -v "$OUT:/out" "$IMAGE" bash -c '
set -euo pipefail
dnf -q -y install wireless-regdb >/dev/null
cp /usr/lib/firmware/regulatory.db /usr/lib/firmware/regulatory.db.p7s /out/
rpm -q wireless-regdb > /out/VERSION
'
cat "$OUT/VERSION"
( cd "$OUT" && shasum -a 256 regulatory.db regulatory.db.p7s )
