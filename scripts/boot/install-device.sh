#!/usr/bin/env bash
# Install the warm-reboot layer on the running Omarchy iPad, over SSH
# (scripts/omarchy/ssh.sh), the way scripts/omarchy/hot-apply.sh does:
#
#   scripts/boot/install-device.sh           install and print linux-a9-kexec status
#   scripts/boot/install-device.sh --check   only print the status
#
# Installs (docs/boot/warm-reboot.md):
#   /usr/local/bin/linux-a9-kexec, /usr/local/lib/linux-a9/{linux-a9-fdt-merge,
#     linux-a9-m1n1-trampoline,phoenix-init,VERSION}
#   /usr/local/bin/linux-a9-update (docs/boot/kernel-updates.md),
#     linux-a9-update-health.service (enabled; inert without a pending trial),
#     /etc/linux-a9/update.conf (only if missing), /etc/linux-a9/health.d,
#     /etc/linux-a9/update-signers from scripts/boot/device/update-signers or
#     build/boot/update-signers (the bundle signing public key), if present
#   kexec-tools (pacman; offline from build/boot/pkgs when the iPad has no
#     internet: scripts/omarchy/fetch-live-pkgs.sh --out build/boot/pkgs kexec-tools)
#   systemd: systemd-reboot.service drop-in (reboot = kexec), system.conf.d
#     watchdog off, linux-a9-kexec-panic.service (enabled)
#   /etc/linux-a9/kexec.conf (only if missing), /boot/linux-a9/entries
#   Omarchy menu: "Reboot Linux (warm)" and "Reboot to iOS…", Update >
#     "iPad Kernel" and "Roll Back iPad Kernel" in
#     ~/.config/omarchy/extensions/omarchy-menu.jsonc
# Nothing is rebooted or loaded. The reboot hook only takes effect once an
# entry exists (scripts/boot/stage-kernel.sh NAME --default); until then a
# reboot halts instead of booting iOS.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
U="${OMARCHY_USER:-linux}"
[[ "$U" =~ ^[a-z_][a-z0-9_-]*$ ]] || { echo 'invalid OMARCHY_USER' >&2; exit 2; }
D="$ROOT/scripts/boot/device"

if [ "${1:-}" = --check ]; then
	exec "$SSH" 'linux-a9-kexec status; linux-a9-update status'
fi
[ -z "${1:-}" ] || { echo "usage: $0 [--check]" >&2; exit 2; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/bin" "$stage/lib" "$stage/systemd" "$stage/pkgs"
cp "$D/linux-a9-kexec" "$D/linux-a9-update" "$stage/bin/"
cp "$D/linux-a9-fdt-merge" "$D/linux-a9-m1n1-trampoline" "$D/phoenix-init" "$D/phoenix-storage" "$D/linux-a9-pstore" "$D/VERSION" "$stage/lib/"
cp "$D"/systemd/* "$stage/systemd/"
cp "$ROOT/scripts/omarchy/rootfs/files/etc/sysctl.d/30-linux-a9-recovery.conf" "$stage/"
cp "$D/omarchy-menu.jsonc" "$D/omarchy-menu-update.jsonc" "$D/update.conf" "$stage/"
for f in "$D/update-signers" "$ROOT/build/boot/update-signers"; do
	[ -s "$f" ] && { cp "$f" "$stage/update-signers"; break; }
done
[ -d "$ROOT/build/boot/pkgs" ] && cp "$ROOT"/build/boot/pkgs/* "$stage/pkgs/" 2>/dev/null || true

COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$stage" -cf - . |
	"$SSH" 'rm -rf /tmp/linux-a9-boot && mkdir -p /tmp/linux-a9-boot && tar -xf - -C /tmp/linux-a9-boot'

"$SSH" "sudo env U=$U bash -s" <<'EOF'
set -euo pipefail
S=/tmp/linux-a9-boot
missing=0
for tool in kexec python3 cpio gzip flock sha256sum ldd zstd ssh-keygen ip findmnt; do
	command -v "$tool" >/dev/null || missing=1
done
if [ "$missing" = 1 ]; then
	if [ -s $S/pkgs/list.txt ]; then
		(cd $S/pkgs && sha256sum -c --quiet SHA256SUMS && pacman -U --needed --noconfirm $(cat list.txt))
	else
		pacman -S --needed --noconfirm kexec-tools python cpio gzip util-linux coreutils glibc zstd openssh iproute2
	fi
fi
for tool in python3 cpio gzip flock sha256sum ldd zstd ssh-keygen ip findmnt; do
	command -v "$tool" >/dev/null || { echo "missing $tool (install python cpio gzip util-linux coreutils glibc zstd openssh iproute2)" >&2; exit 1; }
done
install -Dm0755 $S/bin/linux-a9-kexec /usr/local/bin/linux-a9-kexec
install -Dm0755 $S/bin/linux-a9-update /usr/local/bin/linux-a9-update
install -d /usr/local/lib/linux-a9 /etc/linux-a9 /etc/linux-a9/health.d /boot/linux-a9/entries /boot/linux-a9/update
install -m0755 $S/lib/linux-a9-fdt-merge $S/lib/linux-a9-m1n1-trampoline $S/lib/phoenix-init $S/lib/phoenix-storage $S/lib/linux-a9-pstore /usr/local/lib/linux-a9/
install -m0644 $S/lib/VERSION /usr/local/lib/linux-a9/VERSION
[ -e /etc/linux-a9/update.conf ] || install -m0644 $S/update.conf /etc/linux-a9/update.conf
[ ! -s $S/update-signers ] || install -m0644 $S/update-signers /etc/linux-a9/update-signers
[ -e /etc/linux-a9/kexec.conf ] || cat >/etc/linux-a9/kexec.conf <<'CONF'
# linux-a9-kexec settings (docs/boot/warm-reboot.md)
# crashkernel= added to warm cmdlines that lack one ("" = don't add)
CRASHKERNEL=256M
# seconds the crash kernel lets drivers reset the hardware before the kexec
PHOENIX_DELAY=5
CONF
install -Dm0644 $S/systemd/50-linux-a9-warm.conf /etc/systemd/system/systemd-reboot.service.d/50-linux-a9-warm.conf
install -Dm0644 $S/systemd/50-linux-a9-watchdog.conf /etc/systemd/system.conf.d/50-linux-a9-watchdog.conf
install -Dm0644 $S/systemd/linux-a9-kexec-panic.service /etc/systemd/system/linux-a9-kexec-panic.service
install -Dm0644 $S/systemd/linux-a9-update-health.service /etc/systemd/system/linux-a9-update-health.service
install -Dm0644 $S/systemd/linux-a9-pstore.service /etc/systemd/system/linux-a9-pstore.service
install -Dm0644 $S/30-linux-a9-recovery.conf /etc/sysctl.d/30-linux-a9-recovery.conf
# Ignore absent optional detectors; apply the supported safe defaults now.
sysctl -q -e -p /etc/sysctl.d/30-linux-a9-recovery.conf
systemctl daemon-reload   # also re-reads system.conf.d (the watchdog settings; status shows them)
systemctl enable linux-a9-kexec-panic.service linux-a9-update-health.service linux-a9-pstore.service
install -d /etc/sysctl.d
printf 'kernel.panic = 0\n' >/etc/sysctl.d/90-linux-a9-kexec.conf
sysctl -q -p /etc/sysctl.d/90-linux-a9-kexec.conf
# The watchdog must not be running now either
[ "$(cat /sys/class/watchdog/watchdog0/state 2>/dev/null || echo inactive)" = inactive ] ||
	echo "WARNING: watchdog0 is active: something armed it; a hang will reset into iOS"

# Omarchy menu: user extension file, each snippet added once (keyed by its first id)
home=$(getent passwd "$U" | cut -d: -f6)
m="$home/.config/omarchy/extensions/omarchy-menu.jsonc"
add_menu() {
	grep -q "\"$1\"" "$m" 2>/dev/null && return 0
	install -d -o "$U" -g "$U" "$(dirname "$m")"
	if [ -s "$m" ]; then
		python3 - "$m" "$2" <<'PY'
import sys
path, snippet = sys.argv[1], open(sys.argv[2]).read()
s = open(path).read()
i = s.rindex("}")
body = s[:i].rstrip()
separator = "" if body.endswith(("{", ",")) else ","
open(path, "w").write(body + separator + "\n" + snippet + s[i:])
PY
	else
		printf '{\n%s}\n' "$(cat "$2")" >"$m"
	fi
	chown "$U:$U" "$m"
}
if [ -n "$home" ]; then
	add_menu system.ios $S/omarchy-menu.jsonc
	add_menu update.ipad-kernel $S/omarchy-menu-update.jsonc
fi
rm -rf $S
echo "installed; menu: $(grep -c '"system\.\|"update\.ipad' "$m" 2>/dev/null || echo 0) linux-a9 entries in $m"
[ -s /etc/linux-a9/update-signers ] ||
	echo "note: no /etc/linux-a9/update-signers: remote kernel bundles will be refused (docs/boot/kernel-updates.md)"
EOF
"$SSH" 'linux-a9-kexec status; linux-a9-update status'
