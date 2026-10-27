#!/usr/bin/env bash
# SSH into the jailbroken iPad (iOS side) over USB, no Wi-Fi needed.
# Starts an iproxy tunnel (Mac :2022 -> iPad :22) if one isn't running.
# Key auth for mobile@ is set up (~/.ssh/id_ed25519); sudo on the iPad needs
# the owner's password.
#
# Usage: scripts/ipad-ssh.sh [ssh args / remote command]
#        scripts/ipad-ssh.sh 'ls /usr/share/firmware'
#        IPAD_SCP=1 scripts/ipad-ssh.sh mobile@127.0.0.1:/path local   # scp mode
set -euo pipefail

PORT=2022
if ! nc -z -G 1 127.0.0.1 "$PORT" 2>/dev/null; then
	(iproxy "$PORT:22" >/tmp/linux-a9-iproxy.log 2>&1 &)
	for _ in 1 2 3 4 5; do nc -z -G 1 127.0.0.1 "$PORT" 2>/dev/null && break; sleep 1; done
fi

OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8)
if [ "${IPAD_SCP:-0}" = 1 ]; then
	exec scp "${OPTS[@]}" -P "$PORT" "$@"
fi
exec ssh "${OPTS[@]}" -p "$PORT" mobile@127.0.0.1 "$@"
