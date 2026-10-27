#!/usr/bin/env bash
# Target/rootfs customization. root: install.sh USER ROOTFS-SOURCES MATRIX.json
# AUTOROTATE_CHROOT=1 skips live udev/systemd/session operations.
set -euo pipefail
test "$(id -u)" = 0
U=${1:?user} S=${2:?rootfs sources} MATRIX=${3:?matrix JSON}
home=$(getent passwd "$U" | cut -d: -f6)
uid=$(id -u "$U")
test -n "$home" && test -f "$home/.config/hypr/hyprland.lua"
stamp=$(date -u +%Y%m%dT%H%M%S)-$$
matrix=$(python3 - "$MATRIX" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d['schema'] == 1 and d['name'] == 'apple-a9-aop-accel'
assert d['frame'] == 'iio-sensor-proxy-3.9'
m = d['matrix']
assert len(m) == 3 and all(len(r) == 3 for r in m)
assert all(type(v) is int and v in (-1, 0, 1) for r in m for v in r)
assert all(sum(abs(v) for v in r) == 1 for r in m)
assert all(sum(abs(m[r][c]) for r in range(3)) == 1 for c in range(3))
print(';'.join(','.join(map(str, r)) for r in m))
PY
)
pacman -S --needed --noconfirm iio-sensor-proxy
install -d /etc/linux-a9 /etc/udev/rules.d /etc/systemd/user/ipad-autorotate.service.d
for p in /etc/linux-a9/aop-accel-matrix /etc/linux-a9/aop-accel-matrix.json /etc/udev/rules.d/70-linux-a9-aop-accel.rules; do
  test ! -e "$p" || cp -a "$p" "$p.bak-$stamp"
done
install -m 0644 "$MATRIX" /etc/linux-a9/aop-accel-matrix.json
printf '%s\n' "$matrix" > /etc/linux-a9/aop-accel-matrix
# Upstream 80-iio-sensor-proxy.rules supplies types, systemd tag and Wants.
# An OF platform sensor has no useful DMI hwdb match; use its exact IIO name.
printf 'ACTION!="remove", SUBSYSTEM=="iio", ATTR{name}=="apple-a9-aop-accel", ENV{ACCEL_MOUNT_MATRIX}="%s"\n' "$matrix" > /etc/udev/rules.d/70-linux-a9-aop-accel.rules
install -m 0755 "$S/files/usr/local/bin/"ipad-{rotate,autorotate} /usr/local/bin/
install -m 0644 "$S/files/etc/systemd/user/ipad-autorotate.service" /etc/systemd/user/
# Explicit proxy mode makes acceptance tests exercise D-Bus, rather than silently
# falling back to raw sysfs. The direct reader remains available for diagnosis.
cat > /etc/systemd/user/ipad-autorotate.service.d/20-aop-proxy.conf <<'EOF'
[Unit]
# AOP can register after graphical login; a failed login-time condition is
# never retried by systemd. monitor-sensor waits/reconnects to the proxy.
ConditionPathExistsGlob=

[Service]
ExecStart=
ExecStart=/usr/local/bin/ipad-autorotate --source proxy
EOF
as_user() { runuser -u "$U" -- "$@"; }
as_user install -d "$home/.config/linux-a9"
for p in "$home/.config/hypr/ipad.lua" "$home/.config/hypr/hyprland.lua" "$home/.config/linux-a9/autorotate.conf" "$home/.local/state/linux-a9/display"; do
  test ! -e "$p" || as_user cp -a "$p" "$p.bak-$stamp"
done
as_user install -m 0644 "$S/hypr/ipad.lua" "$home/.config/hypr/ipad.lua"
as_user python3 - "$home" <<'PY'
from pathlib import Path
import re, sys
home = Path(sys.argv[1])
p = home / '.config/hypr/hyprland.lua'
s = p.read_text()
if not re.search(r'''require\s*\(?\s*['"]hypr\.ipad['"]''', s):
    p.write_text(s + '\nrequire("hypr.ipad")\n')
p = home / '.config/linux-a9/autorotate.conf'
s = p.read_text() if p.exists() else ''
# One common system matrix, applied once by either source. Keep timing choices.
s = re.sub(r'^\s*(mount_matrix|normal|bottom-up|right-up|left-up)\s*=.*\n?', '', s, flags=re.M)
p.write_text(s.rstrip() + '\nnormal = portrait\nbottom-up = portrait-flipped\nright-up = landscape\nleft-up = landscape-flipped\n')
p = home / '.local/state/linux-a9/display'
if p.exists():
    # The N1 driver already exports calibrated portrait coordinates.
    p.write_text(re.sub(r'^\s*touch_transform\s*=.*\n?', '', p.read_text(), flags=re.M))
PY
systemctl --global enable ipad-autorotate.service
if [[ ${AUTOROTATE_CHROOT:-0} != 1 ]]; then
  udevadm control --reload-rules
  udevadm trigger --action=change --subsystem-match=iio
  udevadm settle
  # The system service is static/D-Bus activated; do not 'enable' it.
  systemctl restart iio-sensor-proxy.service
  session() { as_user env XDG_RUNTIME_DIR="/run/user/$uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" "$@"; }
  session systemctl --user daemon-reload
  session systemctl --user restart ipad-autorotate.service
  session ipad-rotate "$(session ipad-rotate)"
  session systemctl --user --no-pager status ipad-autorotate.service
fi
echo "Installed proxy + paired display/touch autorotation; matrix=$matrix"
