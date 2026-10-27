#!/usr/bin/env bash
# Run as root on the target, or in its chroot with USB_HOST_CHROOT=1.
# install.sh [user] [path-to-staged-rootfs]; does not switch USB roles.
set -euo pipefail
U=${1:-linux}
SOURCE=${2:-$(cd "$(dirname "$0")/.." && pwd)}
FILES=$SOURCE/files
[[ "$U" =~ ^[a-z_][a-z0-9_-]*$ ]] || exit 2
test "$(id -u)" = 0
test "$(uname -m)" = aarch64
test -f /etc/arch-release
getent passwd "$U" >/dev/null
if [ -s "$SOURCE/packages/list.txt" ]; then
    (
        cd "$SOURCE/packages"
        sha256sum --check --quiet SHA256SUMS
        mapfile -t bundle < list.txt
        for package in "${bundle[@]}"; do
            [[ "$package" =~ ^[a-zA-Z0-9@._+:-]+\.pkg\.tar\.(xz|zst|gz)$ ]] || exit 2
        done
        pacman -U --needed --noconfirm "${bundle[@]}"
    )
fi
packages=(udisks2 udiskie nautilus gvfs polkit usbutils exfatprogs dosfstools ntfs-3g)
missing=()
for package in "${packages[@]}"; do
    pacman -Q "$package" >/dev/null 2>&1 || missing+=("$package")
done
if [ "${#missing[@]}" -gt 0 ]; then
    pacman -S --needed --noconfirm "${missing[@]}"
fi
for file in etc/systemd/user/linux-a9-udiskie.service etc/linux-a9/udiskie.yml \
    etc/systemd/journald.conf.d/91-linux-a9-usb-host.conf \
    usr/local/share/applications/ipad-usb-drives.desktop; do
    install -D -m 0644 "$FILES/$file" "/$file"
done
for file in usr/local/sbin/linux-a9-usb-host usr/local/sbin/linux-a9-iap-roleswitch \
    usr/local/bin/ipad-usb-check usr/local/bin/udiskie; do
    install -D -m 0755 "$FILES/$file" "/$file"
done
install -d -m 0755 /etc/polkit-1/rules.d /var/log/journal
# Scope the unattended user service to USB filesystem mounts only. No disk
# formatting, partitioning, internal mount, or blanket UDisks authorization.
cat > /etc/polkit-1/rules.d/49-linux-a9-usb-mount.rules <<EOF
polkit.addRule(function(action, subject) {
    if (subject.user === "$U" &&
        action.lookup("drive.removable.bus") === "usb" &&
        (action.id === "org.freedesktop.udisks2.filesystem-mount" ||
         action.id === "org.freedesktop.udisks2.filesystem-mount-other-seat")) {
        return polkit.Result.YES;
    }
});
EOF
chmod 0644 /etc/polkit-1/rules.d/49-linux-a9-usb-mount.rules
uid=$(id -u "$U")
user_home=$(getent passwd "$U" | cut -d: -f6)
install -d -o "$U" -g "$(id -gn "$U")" "$user_home/.config/systemd/user/graphical-session.target.wants"
ln -sfn /etc/systemd/user/linux-a9-udiskie.service \
    "$user_home/.config/systemd/user/graphical-session.target.wants/linux-a9-udiskie.service"
chown -h "$U:$(id -gn "$U")" "$user_home/.config/systemd/user/graphical-session.target.wants/linux-a9-udiskie.service"
systemd-tmpfiles --create --prefix /var/log/journal
if [ "${USB_HOST_CHROOT:-0}" != 1 ]; then
    systemctl start udisks2.service
    systemctl restart systemd-journald.service
    journalctl --flush
    if [ -S "/run/user/$uid/bus" ]; then
        user_systemctl() {
            runuser -u "$U" -- env XDG_RUNTIME_DIR="/run/user/$uid" \
                DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" systemctl --user "$@"
        }
        user_systemctl daemon-reload
        user_systemctl stop linux-a9-udiskie.service
        # Stop the already-running stock Omarchy autostart once. Future stock
        # autostarts go through /usr/local/bin/udiskie and join this service.
        while read -r pid; do
            [ -z "$pid" ] || kill -TERM "$pid"
        done < <(pgrep -u "$uid" -x udiskie || true)
        user_systemctl start linux-a9-udiskie.service
        user_systemctl --no-pager status linux-a9-udiskie.service
    else
        echo 'User bus absent: log into Hyprland to start the automounter.'
    fi
fi
printf 'Installed USB automount for %s. USB Drives is in the app launcher.\n' "$U"
