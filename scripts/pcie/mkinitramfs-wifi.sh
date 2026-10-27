#!/usr/bin/env bash
# Put the BCM4355 C0 ("FearA", USI) Wi-Fi firmware into an initramfs.
#
#   scripts/pcie/mkinitramfs-wifi.sh [BASE.gz] [OUT.gz]
#
# BASE defaults to the prebuilt HoolockRD initramfs, OUT to
# build/kernel-pcie/initramfs-wifi.gz. Also writes the firmware-only
# overlay build/kernel-pcie/initramfs-wifi-overlay.gz, for merging into
# another workstream's initramfs with scripts/merge-initramfs.py.
#
# Firmware: build/firmware/lib/firmware/brcm/brcmfmac4355-pcie.apple,feara-
# YSBC-u-4.1{,-XX}.{bin,txt,clm_blob,txcap_blob}, staged from the device
# (variant verified against the unit's HWIdentifiers, see
# build/firmware/selection.json). brcmfmac asks for
# <prefix>.apple,feara-<module>-<vendor>-<version>-<sku> first (OTP read
# OK), then shorter names down to <prefix>.apple,feara, then <prefix>.
# The two shortest names are added as copies of the -XX variant in case
# the OTP read fails.
#
# Also adds /lib/firmware/regulatory.db{,.p7s} (scripts/pcie/fetch-regdb.sh,
# run if missing): cfg80211 is built in and loads it ~1 s into boot.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="${1:-$ROOT/build/prebuilt/initramfs/initramfs.gz}"
OUT="${2:-$ROOT/build/kernel-pcie/initramfs-wifi.gz}"
FW="$ROOT/build/firmware/lib/firmware/brcm"
PREFIX="brcmfmac4355-pcie"
FULL="$PREFIX.apple,feara-YSBC-u-4.1-XX"
OVERLAY="$(dirname "$OUT")/initramfs-wifi-overlay.gz"
REGDB="$ROOT/build/firmware/regdb"

[ -f "$REGDB/regulatory.db.p7s" ] || "$ROOT/scripts/pcie/fetch-regdb.sh" >/dev/null

for ext in bin txt clm_blob txcap_blob; do
	for name in "$PREFIX.apple,feara-YSBC-u-4.1-XX" "$PREFIX.apple,feara-YSBC-u-4.1"; do
		[ -f "$FW/$name.$ext" ] || { echo "missing $FW/$name.$ext" >&2; exit 1; }
	done
done
[ -f "$BASE" ] || { echo "missing base initramfs $BASE" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"

# newc writer: (archive path, source file or None for a directory)
python3 -I - "$FW" "$PREFIX" "$FULL" "$OVERLAY" "$REGDB" <<'EOF'
import gzip, os, sys

fw, prefix, full, out, regdb = sys.argv[1:6]
entries = [("lib", None), ("lib/firmware", None), ("lib/firmware/brcm", None)]
for f in ("regulatory.db", "regulatory.db.p7s"):
    entries.append(("lib/firmware/" + f, os.path.join(regdb, f)))
for f in sorted(os.listdir(fw)):
    if f.startswith(prefix + "."):
        entries.append(("lib/firmware/brcm/" + f, os.path.join(fw, f)))
for ext in ("bin", "txt", "clm_blob", "txcap_blob"):
    src = os.path.join(fw, f"{full}.{ext}")
    entries.append((f"lib/firmware/brcm/{prefix}.apple,feara.{ext}", src))
    if ext != "txcap_blob":
        entries.append((f"lib/firmware/brcm/{prefix}.{ext}", src))

def newc(name, mode, body, ino):
    nb = name.encode() + b"\0"
    hdr = "070701" + "".join(f"{v:08x}" for v in (
        ino, mode, 0, 0, 1, 0, len(body), 0, 0, 0, 0, len(nb), 0))
    rec = hdr.encode() + nb
    rec += b"\0" * (-len(rec) % 4) + body
    return rec + b"\0" * (-len(rec) % 4)

data = b""
for ino, (name, src) in enumerate(entries, 1):
    if src is None:
        data += newc(name, 0o040755, b"", ino)
    else:
        data += newc(name, 0o100644, open(src, "rb").read(), ino)
data += newc("TRAILER!!!", 0, b"", 0)
with gzip.open(out, "wb", 9) as f:
    f.write(data)
print(f"{out}: {len(entries) - 3} firmware files")
EOF

python3 -I "$ROOT/scripts/merge-initramfs.py" "$OUT" "$BASE" "$OVERLAY"
echo "$OUT"
