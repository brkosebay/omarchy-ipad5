#!/usr/bin/env bash
# Orchestrator only. Establish and prove a separate Wi-Fi SSH path before USB.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH=${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}
boot=$(OMARCHY_HOST=172.16.42.1 "$SSH" 'cat /proc/sys/kernel/random/boot_id')
wifi_ip=$(OMARCHY_HOST=172.16.42.1 "$SSH" 'bash -s' <<'REMOTE'
set -euo pipefail
iface=$(nmcli -t -f DEVICE,TYPE device status | awk -F: '$2=="wifi" {print $1; exit}')
test -n "$iface" || { echo 'No NetworkManager Wi-Fi device' >&2; exit 1; }
sudo -n nmcli radio wifi on >&2
state=$(nmcli -g GENERAL.STATE device show "$iface")
case "$state" in
100*) ;;
*)
    uuid=$(nmcli -t -f UUID,TYPE connection show | awk -F: '$2=="802-11-wireless" {print $1; exit}')
    test -n "$uuid" || { echo 'No saved Wi-Fi profile; configure one on the iPad first' >&2; exit 1; }
    sudo -n nmcli --wait 30 connection up uuid "$uuid" ifname "$iface" >&2
    ;;
esac
ip -o -4 address show dev "$iface" scope global | awk 'NR==1 {split($4,a,"/"); print a[1]}'
REMOTE
)
python3 - "$wifi_ip" <<'PY'
import ipaddress, sys
assert ipaddress.ip_address(sys.argv[1]).version == 4, 'no Wi-Fi IPv4 address'
PY
actual=$(OMARCHY_HOST="$wifi_ip" "$SSH" 'cat /proc/sys/kernel/random/boot_id')
test "$boot" = "$actual" || { echo 'Wi-Fi SSH boot_id differs from tether' >&2; exit 1; }
mkdir -p "$ROOT/build/omarchy"
printf '%s\n' "$wifi_ip" > "$ROOT/build/omarchy/wifi-host"
printf 'Verified Wi-Fi SSH to this boot: %s\n' "$wifi_ip"
printf 'export OMARCHY_HOST=%s\n' "$wifi_ip"
