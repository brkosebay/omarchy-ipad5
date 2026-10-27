#!/usr/bin/env bash
# Orchestrator only: install on the running NVMe root through SSH.
# --matrix-file is the six-pose tool's JSON. --trial-adt uses provisional -I.
# Neither mode changes the kernel, its command line, nor the default entry.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH=${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}
U=${OMARCHY_USER:-linux}
[[ "$U" =~ ^[a-z_][a-z0-9_-]*$ ]] || exit 2
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
case ${1:-} in
  --matrix-file)
    test "$#" = 2
    cp "$2" "$stage/matrix.json"
    python3 - "$stage/matrix.json" <<'PY'
import json, sys
assert json.load(open(sys.argv[1])).get('validated') is True, 'use six-pose output, or explicit --trial-adt'
PY
    ;;
  --trial-adt)
    test "$#" = 1
    printf '%s\n' '{"schema":1,"validated":false,"name":"apple-a9-aop-accel","frame":"iio-sensor-proxy-3.9","matrix":[[-1,0,0],[0,-1,0],[0,0,-1]],"note":"UNVERIFIED ADT candidate; six labelled poses required"}' > "$stage/matrix.json"
    echo 'Installing provisional -I from the ADT hypothesis; confirm all four orientations and six raw poses.' >&2
    ;;
  *) echo "usage: $0 --matrix-file POSES/matrix.json | --trial-adt" >&2; exit 2 ;;
esac
mkdir -p "$stage/rootfs/files/usr/local/bin" "$stage/rootfs/files/etc/systemd/user" "$stage/rootfs/hypr" "$stage/rootfs/autorotate"
cp "$ROOT/scripts/omarchy/rootfs/files/usr/local/bin/"ipad-{rotate,autorotate} "$stage/rootfs/files/usr/local/bin/"
cp "$ROOT/scripts/omarchy/rootfs/files/etc/systemd/user/ipad-autorotate.service" "$stage/rootfs/files/etc/systemd/user/"
cp "$ROOT/scripts/omarchy/rootfs/hypr/ipad.lua" "$stage/rootfs/hypr/"
cp "$ROOT/scripts/omarchy/rootfs/autorotate/install.sh" "$stage/rootfs/autorotate/"
# Remote mktemp path is read back, rather than replacing a shared /tmp tree.
remote=$("$SSH" 'mktemp -d /tmp/linux-a9-autorotate.XXXXXXXX')
[[ "$remote" =~ ^/tmp/linux-a9-autorotate\.[A-Za-z0-9]+$ ]] || exit 1
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$stage" -cf - rootfs matrix.json |
  "$SSH" "tar -xf - -C '$remote'"
"$SSH" "sudo -n bash '$remote/rootfs/autorotate/install.sh' '$U' '$remote/rootfs' '$remote/matrix.json'"
"$SSH" "rm -rf -- '$remote'"
