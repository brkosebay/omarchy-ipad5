#!/usr/bin/env bash
# SSH into Omarchy on the iPad (NBD boot) over the USB link.
#   scripts/omarchy/ssh.sh ['<cmd>']
# OMARCHY_HOST=<wifi ip> uses Wi-Fi instead (e.g. when the USB link is down).
# Without OMARCHY_HOST, if the USB address doesn't answer a ping and
# build/omarchy/wifi-host holds the iPad's Wi-Fi address, that is used instead
# (the USB ECM link can drop after a charger renegotiation while Linux runs on).
# The image takes ~/.ssh/id_ed25519.pub at build time (build-rootfs.sh). The
# host key changes with every fresh image, so it isn't recorded.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST=${OMARCHY_HOST:-172.16.42.1}
if [ -z "${OMARCHY_HOST:-}" ] && [ -s "$ROOT/build/omarchy/wifi-host" ] &&
	! ping -c1 -t1 "$HOST" >/dev/null 2>&1; then
	HOST=$(cat "$ROOT/build/omarchy/wifi-host")
fi
exec ssh -i "$HOME/.ssh/id_ed25519" -o IdentitiesOnly=yes \
	-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-o ConnectTimeout=15 -o LogLevel=ERROR "${OMARCHY_USER:-linux}@$HOST" "$@"
