#!/usr/bin/env bash
# ssh into the QEMU test VM that qemu-smoke.sh --keep leaves on 127.0.0.1:2222.
# It also serves as OMARCHY_HOT_SSH, so hot-apply.sh can install the tablet layer
# in the VM instead of on the iPad:
#
#   OMARCHY_HOT_SSH=scripts/omarchy/qemu/vm-ssh.sh scripts/omarchy/hot-apply.sh
#
#   vm-ssh.sh 'cmd'          run cmd (stdin is passed through, so `bash -s` works)
set -euo pipefail
exec ssh -q -p "${OMARCHY_VM_PORT:-2222}" -i "${OMARCHY_SSH_KEY:-$HOME/.ssh/id_ed25519}" \
	-o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
	"${OMARCHY_USERNAME:-linux}@127.0.0.1" "$@"
