#!/usr/bin/env bash
# Offline target installer in a disposable Docker root. Package/service commands
# are mocked; real install, user config, udev text and paired rotation config run.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
docker run --rm -i --platform linux/arm64 -v "$ROOT:/work:ro" linux-a9-kbuild bash -s <<'CONTAINER'
set -euo pipefail
useradd -m -u 1234 rotate-test
home=/home/rotate-test
install -d -o rotate-test -g rotate-test "$home/.config/hypr" "$home/.config/linux-a9" "$home/.local/state/linux-a9" /tmp/stubs
printf '%s\n' '-- test config' > "$home/.config/hypr/hyprland.lua"
printf '%s\n' 'normal = portrait-flipped' 'mount_matrix = 1,0,0;0,1,0;0,0,1' 'debounce = 0.8' > "$home/.config/linux-a9/autorotate.conf"
printf '%s\n' 'orientation=landscape' 'rotation_lock=on' 'touch_transform=0' > "$home/.local/state/linux-a9/display"
chown -R rotate-test:rotate-test "$home"
touch /tmp/actions; chmod 666 /tmp/actions
for name in pacman systemctl udevadm; do
  cat > "/tmp/stubs/$name" <<'STUB'
#!/bin/sh
printf '%s %s\n' "${0##*/}" "$*" >> /tmp/actions
STUB
  chmod +x "/tmp/stubs/$name"
done
export PATH=/tmp/stubs:$PATH
printf '%s\n' '{"schema":1,"name":"apple-a9-aop-accel","frame":"iio-sensor-proxy-3.9","validated":true,"matrix":[[-1,0,0],[0,-1,0],[0,0,-1]]}' > /tmp/matrix.json
for iteration in 1 2; do
  bash /work/scripts/omarchy/rootfs/autorotate/install.sh rotate-test /work/scripts/omarchy/rootfs /tmp/matrix.json
done
python3 - <<'PY'
from pathlib import Path
h = Path('/home/rotate-test')
assert (h / '.config/hypr/hyprland.lua').read_text().count('require("hypr.ipad")') == 1
c = (h / '.config/linux-a9/autorotate.conf').read_text()
assert c.count('normal = portrait\n') == 1 and 'mount_matrix =' not in c
assert 'debounce = 0.8' in c
assert 'touch_transform=' not in (h / '.local/state/linux-a9/display').read_text()
assert 'rotation_lock=on' in (h / '.local/state/linux-a9/display').read_text()
assert Path('/etc/linux-a9/aop-accel-matrix').read_text().strip() == '-1,0,0;0,-1,0;0,0,-1'
assert 'ACCEL_MOUNT_MATRIX}="-1,0,0;0,-1,0;0,0,-1"' in Path('/etc/udev/rules.d/70-linux-a9-aop-accel.rules').read_text()
assert '--source proxy' in Path('/etc/systemd/user/ipad-autorotate.service.d/20-aop-proxy.conf').read_text()
assert 'ConditionPathExistsGlob=\n' in Path('/etc/systemd/user/ipad-autorotate.service.d/20-aop-proxy.conf').read_text()
lua = (h / '.config/hypr/ipad.lua').read_text()
assert 'transform = ipad_transform' in lua and 'or ipad_transform' in lua
commands = Path('/tmp/actions').read_text()
assert 'udevadm trigger --action=change --subsystem-match=iio' in commands
assert 'systemctl restart iio-sensor-proxy.service' in commands
assert 'systemctl --user restart ipad-autorotate.service' in commands
assert 'enable iio-sensor-proxy' not in commands
print('PASS repeat installer: shared matrix, static proxy, paired Lua transforms, lock/timing retained')
PY
CONTAINER
