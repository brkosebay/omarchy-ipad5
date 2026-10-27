#!/usr/bin/env bash
# Exercise installation twice in a disposable container. Package queries are
# mocked as installed; this does not claim an on-device automount test.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
docker run --rm --platform linux/arm64 -v "$ROOT:/work:ro" linux-a9-alarm:latest bash -euo pipefail -c '
useradd -m linux
mkdir -p /testbin /run/user/0
chmod 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
printf "%s\n" 0123456789abcdef0123456789abcdef > /etc/machine-id
cat > /testbin/pacman <<"EOF"
#!/bin/sh
test "$1" = -Q
EOF
cat > /testbin/systemctl <<"EOF"
#!/bin/sh
printf "%s\n" "$*"
EOF
chmod +x /testbin/*
export PATH=/testbin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin
export USB_HOST_CHROOT=1
bash /work/scripts/omarchy/rootfs/usb-host/install.sh linux /work/scripts/omarchy/rootfs
bash /work/scripts/omarchy/rootfs/usb-host/install.sh linux /work/scripts/omarchy/rootfs
test "$(readlink /home/linux/.config/systemd/user/graphical-session.target.wants/linux-a9-udiskie.service)" = /etc/systemd/user/linux-a9-udiskie.service
test "$(stat -c %U /home/linux/.config/systemd/user/graphical-session.target.wants)" = linux
grep -q "subject.user === \"linux\"" /etc/polkit-1/rules.d/49-linux-a9-usb-mount.rules
test "$(udiskie --automount --no-notify --no-tray)" = "--user start linux-a9-udiskie.service"
cat > /usr/bin/udiskie <<"EOF"
#!/bin/sh
printf "packaged: %s\n" "$*"
EOF
chmod +x /usr/bin/udiskie
test "$(udiskie --help)" = "packaged: --help"
systemd-analyze --user verify /etc/systemd/user/linux-a9-udiskie.service
test -x /usr/local/sbin/linux-a9-usb-host
test -x /usr/local/sbin/linux-a9-iap-roleswitch
test -x /usr/local/bin/ipad-usb-check
grep -q Storage=persistent /etc/systemd/journald.conf.d/91-linux-a9-usb-host.conf
! grep -q filesystem-mount-system /etc/polkit-1/rules.d/49-linux-a9-usb-mount.rules
echo "PASS disposable install: idempotence, user ownership, one autostart, unit validation"
'
