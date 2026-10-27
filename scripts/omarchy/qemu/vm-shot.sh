#!/usr/bin/env bash
# Screenshot of the QEMU VM's (or the iPad's) Hyprland session, upright
# whatever the orientation, via grim over SSH.
#
#   vm-shot.sh OUT.png            OMARCHY_TEST_SSH picks another ssh wrapper
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SSH="${OMARCHY_TEST_SSH:-$HERE/vm-ssh.sh}"
"$SSH" 'export XDG_RUNTIME_DIR=/run/user/$(id -u)
sock=$(ls -t "$XDG_RUNTIME_DIR"/wayland-[0-9] 2>/dev/null | head -1)
WAYLAND_DISPLAY=${sock##*/} grim /tmp/vm-shot.png && base64 </tmp/vm-shot.png; rm -f /tmp/vm-shot.png' |
	{ base64 -d 2>/dev/null || base64 -D; } >"$1"
echo "$1"
