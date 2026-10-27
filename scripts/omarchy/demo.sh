#!/usr/bin/env bash
# Run the proof-of-concept tour on the iPad from the Mac, over SSH: the same
# ipad-demo the iPad's own terminal runs (scripts/omarchy/rootfs/files/usr/
# local/bin/ipad-demo, installed by hot-apply.sh and the image build).
#
#   scripts/omarchy/demo.sh            status, fastfetch terminal, chime, browser
#   scripts/omarchy/demo.sh status     Wi-Fi / Bluetooth / battery notification
#   scripts/omarchy/demo.sh fetch      terminal with fastfetch
#   scripts/omarchy/demo.sh sound      a short chime on the speakers
#   scripts/omarchy/demo.sh browser [URL]
#   scripts/omarchy/demo.sh close      close every window between takes
#
# DEMO_PAUSE=SECONDS sets the gap between the steps of the full tour [6].
# OMARCHY_DEMO_SSH: another ssh wrapper taking a remote command (a QEMU VM).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_DEMO_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
args=""
for a in "$@"; do
	args+=" $(printf '%q' "$a")"
done
exec "$SSH" "DEMO_PAUSE=${DEMO_PAUSE:-6} ipad-demo$args"
