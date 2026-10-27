#!/usr/bin/env bash
# Orchestrator only: repeatable live-NVMe install; no reboot or role change.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH=${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}
U=${OMARCHY_USER:-linux}
[[ "$U" =~ ^[a-z_][a-z0-9_-]*$ ]] || exit 2
stage=$(mktemp -d)
remote=
cleanup() {
    rm -rf -- "$stage"
    if [ -n "$remote" ]; then "$SSH" "rm -rf -- '$remote'" || true; fi
}
trap cleanup EXIT
mkdir -p "$stage/rootfs"
cp -R "$ROOT/scripts/omarchy/rootfs/usb-host" "$stage/rootfs/"
if [ -s "$ROOT/build/usb-host/pkgs/list.txt" ]; then
    cp -R "$ROOT/build/usb-host/pkgs" "$stage/rootfs/packages"
fi
for file in etc/systemd/user/linux-a9-udiskie.service etc/linux-a9/udiskie.yml \
    etc/systemd/journald.conf.d/91-linux-a9-usb-host.conf \
    usr/local/share/applications/ipad-usb-drives.desktop \
    usr/local/sbin/linux-a9-usb-host usr/local/sbin/linux-a9-iap-roleswitch \
    usr/local/bin/ipad-usb-check usr/local/bin/udiskie; do
    mkdir -p "$stage/rootfs/files/$(dirname "$file")"
    cp "$ROOT/scripts/omarchy/rootfs/files/$file" "$stage/rootfs/files/$file"
done
"$SSH" 'test "$(findmnt -n -o SOURCE / | cut -c1-9)" = /dev/nvme' || {
    echo 'USB host trials require an NVMe root; refusing NBD.' >&2; exit 1;
}
remote=$("$SSH" 'mktemp -d /tmp/linux-a9-usb-host.XXXXXXXX')
[[ "$remote" =~ ^/tmp/linux-a9-usb-host\.[A-Za-z0-9]+$ ]] || exit 1
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$stage" -cf - rootfs |
    "$SSH" "tar -xf - -C '$remote'"
"$SSH" "sudo -n bash '$remote/rootfs/usb-host/install.sh' '$U' '$remote/rootfs'"
