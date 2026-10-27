#!/usr/bin/env bash
# Boot-owned overlay: early USB logging/rescue first, deferred PCI ANS second.
# Leaves the NVMe workstream's initramfs and source /init untouched.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="${BASE_INITRD:-$ROOT/build/nvme-install/initramfs-omarchy.gz}"
OUT="$ROOT/build/boot"
mkdir -p "$OUT"
[ -f "$BASE" ] || { echo "missing $BASE (scripts/nvme-install/build-initramfs.sh)" >&2; exit 1; }
docker run --rm --platform linux/arm64 -v "$ROOT:/work:ro" -v "$BASE:/base.gz:ro" \
  -v "$OUT:/out" linux-a9-kbuild bash -euo pipefail -c '
mkdir -p /tmp/base /tmp/overlay
cd /tmp/base
gzip -dc /base.gz | cpio -id --quiet
python3 - <<"PY"
from pathlib import Path
# Use the current NVMe-aware init source; a cached base image may still
# carry an older NBD-only /init. Firmware and all other base files stay put.
p=Path("/work/scripts/omarchy/initramfs/init"); s=p.read_text()
# A storage failure must reach rescue after a bounded wait, even with no
# Mac/NBD server. Explicit omarchy.fallback=nbd still overrides this default.
assert s.count("fallback=nbd\n")==1
s=s.replace("fallback=nbd\n", "fallback=rescue\n", 1)
a="charger_limit\nstart_shells\n"
assert s.count(a)==1, "base init changed: early-USB insertion anchor missing"
s=s.replace(a,a+". /linux-a9-early-storage.sh\n"
    "case \"$nvme_wait\" in *[!0-9]*|\"\") nvme_wait=60 ;; esac\n"
    "[ \"$nvme_wait\" -le 120 ] || nvme_wait=120\n"
    "linux_a9_early_storage || rescue \"early USB failed; PCIe/ANS held\"\n",1)
a="nvme_fallback() {\n"
assert s.count(a)==1
s=s.replace(a,a+"  linux_a9_storage_status\n",1)
a="rescue() {\n"
assert s.count(a)==1
s=s.replace(a,a+"  command -v linux_a9_rescue_evidence >/dev/null || . /linux-a9-early-storage.sh\n"
    "  linux_a9_rescue_evidence\n",1)
a="  stop_shells\n  [ -n \"$dhcp_pid\" ]"
assert s.count(a)==1, "base init changed: cleanup anchor missing"
s=s.replace(a,"  linux_a9_early_storage_stop\n"+a,1)
Path("/tmp/overlay/init").write_text(s)
PY
install -m0755 /work/scripts/boot/device/linux-a9-pstore /tmp/overlay/linux-a9-pstore
install -m0755 /work/scripts/boot/device/linux-a9-ans-state /tmp/overlay/linux-a9-ans-state
install -m0755 /work/scripts/boot/device/early-storage.sh /tmp/overlay/linux-a9-early-storage.sh
chmod 0755 /tmp/overlay/init
/tmp/base/lib/ld-musl-aarch64.so.1 /tmp/base/bin/busybox sh -n /tmp/overlay/init
/tmp/base/lib/ld-musl-aarch64.so.1 /tmp/base/bin/busybox sh -n /tmp/overlay/linux-a9-early-storage.sh
/tmp/base/lib/ld-musl-aarch64.so.1 /tmp/base/bin/busybox sh -n /tmp/overlay/linux-a9-ans-state
for a in cat ip sleep ping; do
  /tmp/base/lib/ld-musl-aarch64.so.1 /tmp/base/bin/busybox --list | grep -x "$a" >/dev/null
done
cd /tmp/overlay
find . -type f | cpio -o -H newc -R 0:0 --quiet | gzip -9n >/out/.early-usb-overlay.gz'
python3 -I "$ROOT/scripts/merge-initramfs.py" "$OUT/initramfs-omarchy.gz" "$BASE" "$OUT/.early-usb-overlay.gz"
rm -f "$OUT/.early-usb-overlay.gz"
echo "built $OUT/initramfs-omarchy.gz (early USB before ANS)"
